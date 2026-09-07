#include "../hal/include/macfw_fw1814_hal_shm.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
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

bool halPlaybackReady() {
    const int fd = shm_open(macfw::fw1814::hal::kPlaybackShmName, O_RDWR, 0);
    if (fd < 0) return false;

    void* p = mmap(nullptr, sizeof(macfw::fw1814::hal::SharedPlaybackRing),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;

    auto* ring = static_cast<macfw::fw1814::hal::SharedPlaybackRing*>(p);
    const bool ready = macfw::fw1814::hal::valid(*ring) &&
                       ring->sampleRate.load(std::memory_order_acquire) == 48000;
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

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    const std::string here = executableDirectory(argc > 0 ? argv[0] : nullptr);
    const std::string initPath = here + "/fw1814init";
    const std::string bootPath = here + "/fwboot1814";
    const std::string enginePath = here + "/fw1814analog48";

    std::printf("macfw fw1814supervisor — resilient fixed 48 kHz transport supervisor\n");
    std::printf("automatic reconnect and guarded bootloader recovery: enabled\n");

    std::chrono::milliseconds retryDelay(250);
    constexpr std::chrono::milliseconds kMaxRetryDelay(4000);
    constexpr std::chrono::milliseconds kReenumerationDelay(1000);
    constexpr std::chrono::milliseconds kPostEngineExitDelay(800);

    while (!gStopRequested) {
        if (!halPlaybackReady()) {
            std::printf("FW1814 HAL shared memory not ready; waiting\n");
            do {
                sleepInterruptibly(std::chrono::milliseconds(250));
            } while (!gStopRequested && !halPlaybackReady());
            if (gStopRequested) break;
            std::printf("FW1814 HAL shared memory ready at 48000 Hz\n");
        }

        std::printf("FW1814 applying validated init-48\n");
        const int initStatus = runChild(initPath, "48000", "--execute");
        if (gStopRequested) break;

        if (initStatus != 0) {
            std::fprintf(stderr,
                         "FW1814 init-48 unavailable/failed with status %d; "
                         "checking guarded bootloader personality\n",
                         initStatus);

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
        std::printf("FW1814 init-48 PASS; starting analog transport engine\n");
        const int engineStatus = runChild(enginePath);
        if (gStopRequested) break;

        std::fprintf(stderr,
                     "FW1814 analog transport engine exited with status %d; "
                     "waiting for FireWire re-enumeration\n",
                     engineStatus);
        sleepInterruptibly(kPostEngineExitDelay);
    }

    std::printf("FW1814 supervisor stopping\n");
    return 0;
}
