#include "../hal/include/macfw_fw1814_hal_shm.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }

constexpr int kBootNoBootloader = 10;
constexpr int kBootFailure = 11;
constexpr int kBootGuardRefused = 12;

std::string executableDirectory(const char* argv0) {
    char resolved[PATH_MAX] = {};
    if (argv0 && realpath(argv0, resolved)) {
        char copy[PATH_MAX] = {};
        std::strncpy(copy, resolved, sizeof(copy) - 1);
        return dirname(copy);
    }
    return ".";
}

bool requestedSampleRate(std::uint32_t& rate) {
    rate = 0;
    const int fd = shm_open(macfw::fw1814::hal::kPlaybackShmName, O_RDWR, 0);
    if (fd < 0) return false;

    void* p = mmap(nullptr, sizeof(macfw::fw1814::hal::SharedPlaybackRing),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;

    auto* ring = static_cast<macfw::fw1814::hal::SharedPlaybackRing*>(p);
    bool ready = macfw::fw1814::hal::valid(*ring);
    if (ready) {
        const std::uint32_t requested =
            ring->sampleRate.load(std::memory_order_acquire);
        ready = requested == 44100 || requested == 48000;
        if (ready) rate = requested;
    }
    munmap(p, sizeof(*ring));
    return ready;
}

void sleepInterruptibly(std::chrono::milliseconds duration) {
    const auto deadline = Clock::now() + duration;
    while (!gStopRequested && Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());
        std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(100)));
    }
}

int runChild(const std::string& path,
             const char* arg1 = nullptr,
             const char* arg2 = nullptr) {
    const pid_t pid = fork();
    if (pid < 0) {
        std::fprintf(stderr, "FW1814 supervisor fork failed for %s: %s\n",
                     path.c_str(), std::strerror(errno));
        return 1;
    }

    if (pid == 0) {
        if (arg1 && arg2)
            execl(path.c_str(), path.c_str(), arg1, arg2, static_cast<char*>(nullptr));
        else if (arg1)
            execl(path.c_str(), path.c_str(), arg1, static_cast<char*>(nullptr));
        else
            execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            return 1;
        }

        if (gStopRequested) {
            kill(pid, SIGTERM);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

bool restoreControlState(const std::string& path) {
    if (access(path.c_str(), X_OK) != 0) {
        std::fprintf(stderr,
                     "FW1814 control-state helper unavailable: %s\n",
                     path.c_str());
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execl(path.c_str(), path.c_str(), "restore",
              static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int runEngine(const std::string& path,
              const std::string& stateHelper,
              std::uint32_t startedRate,
              bool& rateChangeRequested) {
    rateChangeRequested = false;
    int readyPipe[2] = {-1, -1};
    if (pipe(readyPipe) != 0) {
        std::fprintf(stderr, "FW1814 supervisor ready-pipe failed: %s\n",
                     std::strerror(errno));
        return 1;
    }
    const int flags = fcntl(readyPipe[0], F_GETFL, 0);
    if (flags >= 0)
        fcntl(readyPipe[0], F_SETFL, flags | O_NONBLOCK);

    const pid_t pid = fork();
    if (pid < 0) {
        close(readyPipe[0]);
        close(readyPipe[1]);
        std::fprintf(stderr, "FW1814 supervisor fork failed for %s: %s\n",
                     path.c_str(), std::strerror(errno));
        return 1;
    }

    if (pid == 0) {
        close(readyPipe[0]);
        char fdText[32] = {};
        std::snprintf(fdText, sizeof(fdText), "%d", readyPipe[1]);
        setenv("MACFW_ENGINE_READY_FD", fdText, 1);
        execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(readyPipe[1]);
    readyPipe[1] = -1;

    std::printf("FW1814 native engine started (pid %d); waiting for READY\n",
                static_cast<int>(pid));

    int status = 0;
    bool stateRestoreAttempted = false;
    for (;;) {
        if (!stateRestoreAttempted && readyPipe[0] >= 0) {
            unsigned char value = 0;
            const ssize_t count = read(readyPipe[0], &value, sizeof(value));
            if (count == 1 && value == 1) {
                close(readyPipe[0]);
                readyPipe[0] = -1;
                stateRestoreAttempted = true;
                std::printf("FW1814 native engine reported READY; restoring "
                            "saved control state\n");
                if (restoreControlState(stateHelper))
                    std::printf("FW1814 saved control state restored\n");
                else
                    std::fprintf(stderr,
                                 "FW1814 saved control-state restore failed; "
                                 "audio remains online\n");
            } else if (count == 0) {
                close(readyPipe[0]);
                readyPipe[0] = -1;
            }
        }

        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            if (readyPipe[0] >= 0) close(readyPipe[0]);
            return 1;
        }

        std::uint32_t requestedRate = 0;
        if (!gStopRequested && requestedSampleRate(requestedRate) &&
            requestedRate != startedRate) {
            std::printf("FW1814 rate request changed: %u -> %u Hz; stopping current transport\n",
                        startedRate, requestedRate);
            rateChangeRequested = true;
            kill(pid, SIGTERM);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }

        if (gStopRequested) {
            kill(pid, SIGTERM);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (readyPipe[0] >= 0) close(readyPipe[0]);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

int runBusReset(const std::string& path) {
    const pid_t pid = fork();
    if (pid < 0) {
        std::fprintf(stderr, "FW1814 supervisor fork failed for %s: %s\n",
                     path.c_str(), std::strerror(errno));
        return 1;
    }

    if (pid == 0) {
        execl(path.c_str(), path.c_str(),
              "--product", "FW 1814", "--execute",
              static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            return 1;
        }

        if (gStopRequested) {
            kill(pid, SIGTERM);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    const std::string here = executableDirectory(argc > 0 ? argv[0] : nullptr);
    const std::string initPath = here + "/fw1814init";
    const std::string bootPath = here + "/fwboot1814";
    const std::string busResetPath = here + "/firewirebusreset";
    const std::string engine48Path = here + "/fw1814analog48";
    const std::string engine44Path = here + "/fw1814analog44";
    const std::string stateHelperPath = here + "/fw1814state";

    std::printf("macfw fw1814supervisor — resilient 44.1/48 kHz transport supervisor\n");
    std::printf("automatic reconnect and guarded bootloader recovery: enabled\n");
    std::printf("validated pre-transport FW1814 bus reset: enabled\n");
    std::printf("persistent validated routing-state restore: enabled\n");

    std::chrono::milliseconds retryDelay(250);
    constexpr std::chrono::milliseconds kMaxRetryDelay(4000);
    constexpr std::chrono::milliseconds kReenumerationDelay(1000);
    constexpr std::chrono::milliseconds kPostEngineExitDelay(800);
    constexpr std::chrono::milliseconds kCleanBusResetSettleDelay(3000);

    // A physical disconnect/re-enumeration can leave the FW1814 playback side
    // in a bad device-local stream state even though init-48 and all host-side
    // transport diagnostics pass. Hardware testing showed that one guarded,
    // product-scoped bus reset while the operational personality is confirmed,
    // followed by a settle delay and a fresh init-48, reliably clears it.
    //
    // This flag is consumed before the reset is issued so the generation change
    // caused by our own reset cannot recursively request another reset.
    bool cleanBusResetRequired = true;

    while (!gStopRequested) {
        std::uint32_t requestedRate = 0;
        if (!requestedSampleRate(requestedRate)) {
            std::printf("FW1814 HAL shared memory not ready; waiting\n");
            do {
                sleepInterruptibly(std::chrono::milliseconds(250));
            } while (!gStopRequested && !requestedSampleRate(requestedRate));
            if (gStopRequested) break;
            std::printf("FW1814 HAL shared memory ready at %u Hz\n",
                        requestedRate);
        }

        const char* rateArg = requestedRate == 44100 ? "44100" : "48000";
        std::printf("FW1814 applying validated init-%s\n", rateArg);
        const int initStatus = runChild(initPath, rateArg, "--execute");
        if (gStopRequested) break;

        if (initStatus != 0) {
            std::fprintf(stderr,
                         "FW1814 init-%s unavailable/failed with status %d; "
                         "checking guarded bootloader personality\n",
                         rateArg, initStatus);

            const int bootStatus = runChild(bootPath, "--execute");
            if (gStopRequested) break;

            if (bootStatus == 0) {
                std::printf("FW1814 guarded boot cue issued; waiting for re-enumeration\n");
                retryDelay = std::chrono::milliseconds(250);
                sleepInterruptibly(kReenumerationDelay);
                continue;
            }

            if (bootStatus == kBootGuardRefused) {
                std::fprintf(stderr,
                             "FW1814 bootloader guard refused candidate; no write performed; "
                             "retrying in 2 s\n");
                sleepInterruptibly(std::chrono::seconds(2));
                continue;
            }

            if (bootStatus == kBootNoBootloader) {
                std::printf("FW1814 operational/bootloader personality not ready; retrying in %lld ms\n",
                            static_cast<long long>(retryDelay.count()));
            } else if (bootStatus == kBootFailure) {
                std::fprintf(stderr,
                             "FW1814 guarded bootloader check failed; retrying in %lld ms\n",
                             static_cast<long long>(retryDelay.count()));
            } else {
                std::fprintf(stderr,
                             "FW1814 boot helper exited with status %d; retrying in %lld ms\n",
                             bootStatus,
                             static_cast<long long>(retryDelay.count()));
            }

            sleepInterruptibly(retryDelay);
            retryDelay = std::min(retryDelay * 2, kMaxRetryDelay);
            continue;
        }

        retryDelay = std::chrono::milliseconds(250);

        std::uint32_t latestRate = 0;
        if (!requestedSampleRate(latestRate) || latestRate != requestedRate) {
            std::printf("FW1814 rate request changed during init; restarting selection\n");
            continue;
        }

        if (cleanBusResetRequired) {
            std::printf("FW1814 operational init PASS; performing validated clean bus reset before transport\n");

            // Consume the request before issuing BusReset(). Our own reset will
            // change the FireWire generation; that generation change must not
            // schedule another reset recursively.
            cleanBusResetRequired = false;
            const int resetStatus = runBusReset(busResetPath);
            if (gStopRequested) break;

            if (resetStatus != 0) {
                cleanBusResetRequired = true;
                std::fprintf(stderr,
                             "FW1814 guarded bus reset failed with status %d; "
                             "not starting transport; retrying in %lld ms\n",
                             resetStatus,
                             static_cast<long long>(retryDelay.count()));
                sleepInterruptibly(retryDelay);
                retryDelay = std::min(retryDelay * 2, kMaxRetryDelay);
                continue;
            }

            std::printf("FW1814 clean bus reset PASS; waiting 3000 ms for re-enumeration before fresh init-%s\n",
                        rateArg);
            sleepInterruptibly(kCleanBusResetSettleDelay);
            continue;
        }

        const std::string& enginePath =
            requestedRate == 44100 ? engine44Path : engine48Path;
        std::printf("FW1814 post-reset init-%s PASS; starting %s\n",
                    rateArg, requestedRate == 44100
                        ? "44.1 kHz analog transport engine"
                        : "48 kHz analog transport engine");
        bool rateChangeRequested = false;
        const int engineStatus =
            runEngine(enginePath, stateHelperPath, requestedRate,
                      rateChangeRequested);
        if (gStopRequested) break;

        // Any engine exit outside supervisor shutdown means the next transport
        // start must pass through the validated clean bus-reset sequence again.
        cleanBusResetRequired = true;
        if (rateChangeRequested) {
            std::printf("FW1814 controlled rate handoff requested; "
                        "clean bus reset required before next transport start\n");
        } else {
            std::fprintf(stderr,
                         "FW1814 analog transport engine exited with status %d; "
                         "clean bus reset required before next transport start\n",
                         engineStatus);
        }
        sleepInterruptibly(kPostEngineExitDelay);
    }

    std::printf("FW1814 supervisor stopping\n");
    return 0;
}
