#include "../hal/include/macfw_fw1814_hal_shm.h"
#include "../hal/include/macfw_fw1814_high_rate.h"

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

constexpr std::chrono::milliseconds kSupervisorPollInterval(500);

using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }

constexpr int kBootNoBootloader = 10;
constexpr int kBootFailure = 11;
constexpr int kBootGuardRefused = 12;
constexpr int kQualificationRetry = 2;

bool isQuadRate(std::uint32_t rate) {
    return rate == 176400 || rate == 192000;
}

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
        ready = requested == 44100 || requested == 48000 ||
                (requested == 88200 && macfw::fw1814::experimental::enabled88()) ||
                (requested == 96000 && macfw::fw1814::experimental::enabled96()) ||
                (requested == 176400 && macfw::fw1814::experimental::enabled176()) ||
                (requested == 192000 && macfw::fw1814::experimental::enabled192());
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

        std::this_thread::sleep_for(kSupervisorPollInterval);
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
              const std::string& controlHelper,
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
        (void)startedRate;
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
                const int readyStatus =
                    runChild(controlHelper, "internal-ready");
                if (readyStatus == 0)
                    std::printf("FW1814 control state READY\n");
                else
                    std::fprintf(stderr,
                                 "FW1814 could not publish control readiness "
                                 "(status %d)\n",
                                 readyStatus);
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

        std::this_thread::sleep_for(kSupervisorPollInterval);
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

        std::this_thread::sleep_for(kSupervisorPollInterval);
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}


int runFirmwareReboot(const std::string& resetPath,
                      const std::string& bootPath) {
    const int resetStatus =
        runChild(resetPath, "--execute", "--experimental-firmware-reset");
    if (resetStatus != 0) return resetStatus;
    if (gStopRequested) return 130;

    std::printf("FW1814 firmware reset accepted; waiting for bootloader personality\n");
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    sleepInterruptibly(std::chrono::milliseconds(500));

    while (!gStopRequested && Clock::now() < deadline) {
        const int bootStatus = runChild(bootPath, "--execute");
        if (bootStatus == 0) {
            std::printf("FW1814 guarded boot-from-flash cue issued; "
                        "operational personality will be polled by the supervisor\n");
            return gStopRequested ? 130 : 0;
        }

        if (bootStatus != kBootNoBootloader) {
            std::fprintf(stderr,
                         "FW1814 guarded boot-from-flash failed with status %d\n",
                         bootStatus);
            return bootStatus;
        }

        sleepInterruptibly(kSupervisorPollInterval);
    }

    if (gStopRequested) return 130;
    std::fprintf(stderr,
                 "FW1814 bootloader personality did not appear within 10 seconds\n");
    return 124;
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
    const std::string firmwareResetPath = here + "/fw1814firmwarereset";
    const std::string busResetPath = here + "/firewirebusreset";
    const std::string engine48Path = here + "/fw1814analog48";
    const std::string engine44Path = here + "/fw1814analog44";
    const std::string engine96Path = here + "/fw1814analog96";
    const std::string engine88Path = here + "/fw1814analog88";
    const std::string engine176Path = here + "/fw1814analog176";
    const std::string engine192Path = here + "/fw1814analog192";
    const std::string stateHelperPath = here + "/fw1814state";
    const std::string controlHelperPath = here + "/fw1814ctl";

    std::printf("macfw fw1814supervisor — resilient 44.1/48/88.2/96/176.4/192 kHz transport supervisor\n");
    const bool forceRecovery = std::getenv("MACFW_FW1814_FORCE_RECOVERY") != nullptr;
    std::printf("automatic reconnect: enabled\n");
    std::printf("routine pre-transport recovery: %s (set MACFW_FW1814_FORCE_RECOVERY=1 to enable)\n",
                forceRecovery ? "enabled" : "disabled");
    std::printf("persistent validated routing-state restore: enabled\n");

    std::chrono::milliseconds retryDelay(250);
    constexpr std::chrono::milliseconds kMaxRetryDelay(4000);
    constexpr std::chrono::milliseconds kReenumerationDelay(1000);
    constexpr std::chrono::milliseconds kPostEngineExitDelay(800);
    constexpr std::chrono::milliseconds kCleanBusResetSettleDelay(3000);
    constexpr std::chrono::milliseconds kQuadRatePostInitQuiescence(2000);

    // Do not reboot or bus-reset the device as part of ordinary startup. Linux
    // snd-bebob changes rates without a routine recovery; keeping this path
    // opt-in lets malformed first-attempt streams remain observable. A genuine
    // engine failure still arms recovery below.
    bool deviceRecoveryRequired = forceRecovery;
    bool operationalEnumerationPending = false;
    Clock::time_point operationalEnumerationDeadline = Clock::time_point::min();

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
            if (operationalEnumerationPending) {
                if (Clock::now() < operationalEnumerationDeadline) {
                    std::printf("FW1814 operational personality not ready after "
                                "boot-from-flash; polling again in 500 ms\n");
                    sleepInterruptibly(kSupervisorPollInterval);
                    continue;
                }

                operationalEnumerationPending = false;
                deviceRecoveryRequired = true;
                std::fprintf(stderr,
                             "FW1814 operational personality did not return "
                             "within 10 seconds; guarded recovery will retry\n");
                sleepInterruptibly(retryDelay);
                continue;
            }

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
        if (operationalEnumerationPending) {
            const auto enumerationMs = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                Clock::now() -
                (operationalEnumerationDeadline - std::chrono::seconds(10)))
                                           .count();
            std::printf("FW1814 operational personality ready after %lld ms\n",
                        static_cast<long long>(enumerationMs));
            operationalEnumerationPending = false;
        }

        std::uint32_t latestRate = 0;
        if (!requestedSampleRate(latestRate) || latestRate != requestedRate) {
            std::printf("FW1814 rate request changed during init; restarting selection\n");
            continue;
        }

        if (deviceRecoveryRequired) {
            // Consume the request before issuing a reset. Either recovery
            // changes device/bus state and must not schedule itself recursively.
            deviceRecoveryRequired = false;

            if (isQuadRate(requestedRate)) {
                std::printf("FW1814 operational init PASS; performing guarded firmware reboot before %u Hz transport\n",
                            requestedRate);
                const int recoveryStatus =
                    runFirmwareReboot(firmwareResetPath, bootPath);
                if (gStopRequested) break;

                if (recoveryStatus != 0) {
                    deviceRecoveryRequired = true;
                    std::fprintf(stderr,
                                 "FW1814 guarded firmware reboot failed with status %d; "
                                 "not starting transport; retrying in %lld ms\n",
                                 recoveryStatus,
                                 static_cast<long long>(retryDelay.count()));
                    sleepInterruptibly(retryDelay);
                    retryDelay = std::min(retryDelay * 2, kMaxRetryDelay);
                    continue;
                }

                operationalEnumerationPending = true;
                operationalEnumerationDeadline =
                    Clock::now() + std::chrono::seconds(10);
                std::printf("FW1814 guarded firmware reboot PASS; polling for "
                            "fresh operational init-48000\n");
                continue;
            }

            std::printf("FW1814 operational init PASS; performing validated clean bus reset before transport\n");
            const int resetStatus = runBusReset(busResetPath);
            if (gStopRequested) break;

            if (resetStatus != 0) {
                deviceRecoveryRequired = true;
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

        const std::string& enginePath = requestedRate == 192000 ? engine192Path :
            requestedRate == 176400 ? engine176Path :
            requestedRate == 88200 ? engine88Path :
            requestedRate == 96000 ? engine96Path :
            requestedRate == 44100 ? engine44Path : engine48Path;
        if (access(enginePath.c_str(), X_OK) != 0) {
            std::fprintf(stderr, "FW1814 requested engine unavailable: %s\n", enginePath.c_str());
            sleepInterruptibly(retryDelay);
            continue;
        }
        if (isQuadRate(requestedRate)) {
            std::printf("FW1814 post-init 48000 PASS; quiescing device for 2000 ms before %u Hz transport\n",
                        requestedRate);
            sleepInterruptibly(kQuadRatePostInitQuiescence);
            if (gStopRequested) break;

            std::uint32_t settledRate = 0;
            if (!requestedSampleRate(settledRate) || settledRate != requestedRate) {
                std::printf("FW1814 rate request changed during quad-rate quiescence; restarting selection\n");
                continue;
            }
        }
        std::printf("FW1814 post-init %s PASS; starting %s\n",
                    rateArg, requestedRate == 192000
                        ? "192 kHz analog transport engine"
                        : requestedRate == 176400
                        ? "176.4 kHz analog transport engine"
                        : requestedRate == 88200
                        ? "88.2 kHz analog transport engine"
                        : requestedRate == 96000
                        ? "96 kHz analog transport engine"
                        : requestedRate == 44100
                            ? "44.1 kHz analog transport engine"
                            : "48 kHz analog transport engine");
        bool rateChangeRequested = false;
        const int engineStatus =
            runEngine(enginePath, stateHelperPath, controlHelperPath,
                      requestedRate,
                      rateChangeRequested);
        if (gStopRequested) break;

        if (rateChangeRequested) {
            std::printf("FW1814 controlled rate handoff requested; "
                        "restarting without firmware recovery\n");
            // Normal rate changes follow the Linux snd-bebob model: stop the
            // stream, restore the known 48 kHz baseline, apply the new rate,
            // then start the selected engine. Reserve firmware recovery for
            // an engine failure or failed qualification.
            deviceRecoveryRequired = false;
        } else if (engineStatus == kQualificationRetry) {
            // Qualification and startup-schedule rejection are host-side
            // transport failures. A FireWire bus reset cannot repair them and
            // would only turn a deterministic rejection into a reset loop.
            std::printf("FW1814 %u Hz transport qualification/startup failed; "
                        "retrying without firmware recovery\n", requestedRate);
            deviceRecoveryRequired = false;
        } else {
            // Any unexpected engine exit means the next transport start must
            // pass through the rate-appropriate recovery again.
            deviceRecoveryRequired = true;
            std::fprintf(stderr,
                         "FW1814 analog transport engine exited with status %d; "
                         "device recovery required before next transport start\n",
                         engineStatus);
        }
        sleepInterruptibly(kPostEngineExitDelay);
    }

    std::printf("FW1814 supervisor stopping\n");
    return 0;
}
