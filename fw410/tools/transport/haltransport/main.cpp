#include "macfw_hal_shm.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <libgen.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }

constexpr int kFwbootNoBootloader = 10;
constexpr int kFwbootGuardRefused = 11;
constexpr int kFwbootCandidateUnavailable = 12;

using Clock = std::chrono::steady_clock;

enum class SupervisorState {
    Idle,
    Running,
    WaitingForReenumeration,
    Backoff,
};

enum class BootResult {
    CueIssued,
    NoBootloader,
    GuardRefused,
    CandidateUnavailable,
    Failed,
};

std::string executableDirectory(const char* argv0) {
    char resolved[PATH_MAX] = {};
    if (argv0 && realpath(argv0, resolved)) {
        char copy[PATH_MAX] = {};
        std::strncpy(copy, resolved, sizeof(copy) - 1);
        return dirname(copy);
    }
    return ".";
}

class SharedState {
public:
    ~SharedState() {
        if (ring_) munmap(ring_, sizeof(*ring_));
        if (fd_ >= 0) close(fd_);
    }

    bool open() {
        fd_ = shm_open(macfw::hal::kShmName, O_RDWR, 0);
        if (fd_ < 0) return false;
        void* p = mmap(nullptr, sizeof(macfw::hal::SharedPcmRing), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        ring_ = static_cast<macfw::hal::SharedPcmRing*>(p);
        return macfw::hal::valid(*ring_);
    }

    unsigned rate() const {
        return ring_ ? ring_->sampleRate.load(std::memory_order_acquire) : 0;
    }

private:
    int fd_ = -1;
    macfw::hal::SharedPcmRing* ring_ = nullptr;
};

std::string bridgePath(const std::string& here, unsigned rate) {
    if (rate == 44100) return here + "/../halbridge44100/halbridge44100";
    if (rate == 48000) return here + "/../halbridge48000/halbridge48000";
    return {};
}

std::string fwbootPath(const std::string& here) {
    return here + "/../../device/fwboot/fwboot";
}

pid_t startBridge(const std::string& path, unsigned rate) {
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    std::printf("transport: started native %u Hz engine (pid %d)\n",
                rate, static_cast<int>(pid));
    return pid;
}

int stopBridge(pid_t& pid) {
    if (pid <= 0) return 0;
    kill(pid, SIGTERM);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        status = 0;
        break;
    }
    pid = -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

bool childExited(pid_t pid, int& status) {
    if (pid <= 0) return false;
    const pid_t r = waitpid(pid, &status, WNOHANG);
    return r == pid;
}

BootResult tryGuardedBoot(const std::string& path) {
    if (access(path.c_str(), X_OK) != 0) {
        std::fprintf(stderr, "transport: fwboot helper not built: %s\n", path.c_str());
        return BootResult::Failed;
    }

    std::printf("transport: checking FW410 bootloader mode\n");
    const pid_t pid = fork();
    if (pid < 0) return BootResult::Failed;
    if (pid == 0) {
        execl(path.c_str(), path.c_str(), "--execute", static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return BootResult::Failed;
    }

    if (!WIFEXITED(status)) return BootResult::Failed;
    switch (WEXITSTATUS(status)) {
        case 0: return BootResult::CueIssued;
        case kFwbootNoBootloader: return BootResult::NoBootloader;
        case kFwbootGuardRefused: return BootResult::GuardRefused;
        case kFwbootCandidateUnavailable: return BootResult::CandidateUnavailable;
        default: return BootResult::Failed;
    }
}

const char* stateName(SupervisorState state) {
    switch (state) {
        case SupervisorState::Idle: return "IDLE";
        case SupervisorState::Running: return "RUNNING";
        case SupervisorState::WaitingForReenumeration: return "WAIT_REENUMERATION";
        case SupervisorState::Backoff: return "BACKOFF";
    }
    return "UNKNOWN";
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    SharedState shared;
    if (!shared.open()) {
        std::fprintf(stderr, "HAL shared state unavailable; install/restart the HAL plug-in first\n");
        return 1;
    }

    const std::string here = executableDirectory(argc > 0 ? argv[0] : nullptr);
    const std::string bootHelper = fwbootPath(here);

    unsigned activeRate = 0;
    pid_t child = -1;
    SupervisorState state = SupervisorState::Idle;
    Clock::time_point nextAction = Clock::now();
    Clock::time_point childStarted = Clock::time_point::min();
    std::chrono::milliseconds retryDelay(250);
    constexpr std::chrono::milliseconds kMaxRetryDelay(4000);
    constexpr std::chrono::milliseconds kReenumerationDelay(800);
    constexpr std::chrono::seconds kStableRun(5);

    std::printf("macfw haltransport — rate-aware CoreAudio HAL to FW410 transport supervisor\n");
    std::printf("supported native rates: 44100, 48000 Hz\n");
    std::printf("automatic guarded FW410 bootloader recovery: enabled\n");
    std::printf("disconnect/re-enumeration retry backoff: enabled\n");
    std::printf("change the device format in Audio MIDI Setup to switch engines\n");

    while (!gStopRequested) {
        const auto now = Clock::now();
        const unsigned requestedRate = shared.rate();

        if (child > 0 && requestedRate != activeRate) {
            std::printf("transport: HAL rate changed %u -> %u Hz; stopping old engine\n",
                        activeRate, requestedRate);
            stopBridge(child);
            activeRate = 0;
            state = SupervisorState::Idle;
            retryDelay = std::chrono::milliseconds(250);
            nextAction = now;
        }

        int childStatus = 0;
        if (childExited(child, childStatus)) {
            const unsigned failedRate = activeRate;
            const auto runTime = childStarted == Clock::time_point::min()
                ? Clock::duration::zero()
                : now - childStarted;

            std::fprintf(stderr, "transport: native %u Hz engine exited", failedRate);
            if (WIFEXITED(childStatus))
                std::fprintf(stderr, " with status %d", WEXITSTATUS(childStatus));
            else if (WIFSIGNALED(childStatus))
                std::fprintf(stderr, " on signal %d", WTERMSIG(childStatus));
            std::fprintf(stderr, "\n");

            child = -1;
            activeRate = 0;
            childStarted = Clock::time_point::min();

            if (runTime >= kStableRun) retryDelay = std::chrono::milliseconds(250);

            if (!gStopRequested && failedRate != 0 && shared.rate() == failedRate) {
                const BootResult boot = tryGuardedBoot(bootHelper);
                switch (boot) {
                    case BootResult::CueIssued:
                        state = SupervisorState::WaitingForReenumeration;
                        nextAction = now + kReenumerationDelay;
                        retryDelay = std::chrono::milliseconds(250);
                        std::printf("transport: guarded boot cue issued; state=%s\n",
                                    stateName(state));
                        break;
                    case BootResult::NoBootloader:
                        state = SupervisorState::Backoff;
                        nextAction = now + retryDelay;
                        std::printf("transport: no FW410 bootloader present; state=%s retry=%lld ms\n",
                                    stateName(state), static_cast<long long>(retryDelay.count()));
                        retryDelay = std::min(retryDelay * 2, kMaxRetryDelay);
                        break;
                    case BootResult::GuardRefused:
                        state = SupervisorState::Backoff;
                        nextAction = now + std::chrono::seconds(2);
                        std::fprintf(stderr,
                                     "transport: FW410 bootloader candidate failed guard preflight; "
                                     "retrying conservatively\n");
                        break;
                    case BootResult::CandidateUnavailable:
                    case BootResult::Failed:
                        state = SupervisorState::Backoff;
                        nextAction = now + retryDelay;
                        std::fprintf(stderr,
                                     "transport: bootloader check unavailable/failed; retry=%lld ms\n",
                                     static_cast<long long>(retryDelay.count()));
                        retryDelay = std::min(retryDelay * 2, kMaxRetryDelay);
                        break;
                }
            } else {
                state = SupervisorState::Idle;
            }
        }

        if (child <= 0 && requestedRate != 0 && now >= nextAction) {
            const std::string path = bridgePath(here, requestedRate);
            if (path.empty()) {
                std::fprintf(stderr, "transport: HAL selected unsupported rate %u Hz\n",
                             requestedRate);
                state = SupervisorState::Backoff;
                nextAction = now + std::chrono::seconds(1);
            } else if (access(path.c_str(), X_OK) != 0) {
                std::fprintf(stderr, "transport: native %u Hz engine not built: %s\n",
                             requestedRate, path.c_str());
                state = SupervisorState::Backoff;
                nextAction = now + std::chrono::seconds(1);
            } else {
                child = startBridge(path, requestedRate);
                if (child > 0) {
                    activeRate = requestedRate;
                    state = SupervisorState::Running;
                    childStarted = now;
                } else {
                    state = SupervisorState::Backoff;
                    nextAction = now + retryDelay;
                    retryDelay = std::min(retryDelay * 2, kMaxRetryDelay);
                }
            }
        }

        if (child > 0 && childStarted != Clock::time_point::min() &&
            now - childStarted >= kStableRun) {
            retryDelay = std::chrono::milliseconds(250);
        }

        usleep(100000);
    }

    if (child > 0) {
        std::printf("transport: stop requested; stopping native %u Hz engine\n", activeRate);
        stopBridge(child);
    }
    return 0;
}
