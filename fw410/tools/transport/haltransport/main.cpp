#include "macfw_hal_shm.h"
#include "../transport_status_shared.h"

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

enum class SupervisorState { Idle, Running, WaitingForReenumeration, Backoff };
enum class BootResult { CueIssued, NoBootloader, GuardRefused, CandidateUnavailable, Failed };

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
    ~SharedState() { if (ring_) munmap(ring_, sizeof(*ring_)); if (fd_ >= 0) close(fd_); }
    bool open() {
        fd_ = shm_open(macfw::hal::kShmName, O_RDWR, 0);
        if (fd_ < 0) return false;
        void* p = mmap(nullptr, sizeof(macfw::hal::SharedPcmRing), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) { close(fd_); fd_ = -1; return false; }
        ring_ = static_cast<macfw::hal::SharedPcmRing*>(p);
        return macfw::hal::valid(*ring_);
    }
    unsigned rate() const { return ring_ ? ring_->sampleRate.load(std::memory_order_acquire) : 0; }
private:
    int fd_ = -1;
    macfw::hal::SharedPcmRing* ring_ = nullptr;
};

std::string bridgePath(const std::string& here, unsigned rate) {
    if (rate == 44100) return here + "/../halbridge44100/halbridge44100";
    if (rate == 48000) return here + "/../halbridge48000/halbridge48000";
    return {};
}
std::string fwbootPath(const std::string& here) { return here + "/../../device/fwboot/fwboot"; }

struct BridgeProcess {
    pid_t pid = -1;
    int readyFd = -1;
    bool ready = false;
};

void closeReadyFd(BridgeProcess& child) {
    if (child.readyFd >= 0) close(child.readyFd);
    child.readyFd = -1;
}

BridgeProcess startBridge(const std::string& path, unsigned rate) {
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0) return {};
    const int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    const pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return {}; }
    if (pid == 0) {
        close(pipefd[0]);
        char fdText[32] = {};
        std::snprintf(fdText, sizeof(fdText), "%d", pipefd[1]);
        setenv("MACFW_ENGINE_READY_FD", fdText, 1);
        execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipefd[1]);
    std::printf("transport: started native %u Hz engine (pid %d); waiting for ready signal\n", rate, static_cast<int>(pid));
    BridgeProcess child;
    child.pid = pid;
    child.readyFd = pipefd[0];
    return child;
}

bool pollReady(BridgeProcess& child) {
    if (child.pid <= 0 || child.ready || child.readyFd < 0) return child.ready;
    unsigned char value = 0;
    const ssize_t n = read(child.readyFd, &value, sizeof(value));
    if (n == 1 && value == 1) {
        child.ready = true;
        closeReadyFd(child);
        std::printf("transport: native engine reported READY (pid %d)\n", static_cast<int>(child.pid));
    } else if (n == 0) {
        closeReadyFd(child);
    }
    return child.ready;
}

int stopBridge(BridgeProcess& child) {
    closeReadyFd(child);
    if (child.pid <= 0) { child = {}; return 0; }
    kill(child.pid, SIGTERM);
    int status = 0;
    while (waitpid(child.pid, &status, 0) < 0) { if (errno == EINTR) continue; status = 0; break; }
    child = {};
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

bool childExited(BridgeProcess& child, int& status) {
    if (child.pid <= 0) return false;
    const pid_t r = waitpid(child.pid, &status, WNOHANG);
    if (r != child.pid) return false;
    closeReadyFd(child);
    return true;
}

BootResult tryGuardedBoot(const std::string& path) {
    if (access(path.c_str(), X_OK) != 0) { std::fprintf(stderr, "transport: fwboot helper not built: %s\n", path.c_str()); return BootResult::Failed; }
    std::printf("transport: checking FW410 bootloader mode\n");
    const pid_t pid = fork();
    if (pid < 0) return BootResult::Failed;
    if (pid == 0) { execl(path.c_str(), path.c_str(), "--execute", static_cast<char*>(nullptr)); _exit(127); }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) { if (errno == EINTR) continue; return BootResult::Failed; }
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
    std::signal(SIGINT, signalHandler); std::signal(SIGTERM, signalHandler);
    SharedState shared;
    if (!shared.open()) { std::fprintf(stderr, "HAL shared state unavailable; install/restart the HAL plug-in first\n"); return 1; }
    macfw::transport::TransportStatusPublisher transportStatus;
    if (!transportStatus.open()) { std::fprintf(stderr, "transport status shared state unavailable\n"); return 1; }

    const std::string here = executableDirectory(argc > 0 ? argv[0] : nullptr);
    const std::string bootHelper = fwbootPath(here);
    unsigned activeRate = 0;
    BridgeProcess child;
    SupervisorState state = SupervisorState::Idle;
    Clock::time_point nextAction = Clock::now();
    Clock::time_point childStarted = Clock::time_point::min();
    std::chrono::milliseconds retryDelay(250);
    constexpr std::chrono::milliseconds kMaxRetryDelay(4000), kReenumerationDelay(800);
    constexpr std::chrono::seconds kStableRun(5); // retry-backoff reset only; not readiness

    std::printf("macfw haltransport — rate-aware CoreAudio HAL to FW410 transport supervisor\n");
    std::printf("supported native rates: 44100, 48000 Hz\n");
    std::printf("automatic guarded FW410 bootloader recovery: enabled\n");
    std::printf("disconnect/re-enumeration retry backoff: enabled\n");
    std::printf("transport status ABI: explicit native-engine readiness enabled\n");
    std::printf("change the device format in Audio MIDI Setup to switch engines\n");

    while (!gStopRequested) {
        const auto now = Clock::now();
        const unsigned requestedRate = shared.rate();

        if (child.pid > 0 && requestedRate != activeRate) {
            std::printf("transport: HAL rate changed %u -> %u Hz; stopping old engine\n", activeRate, requestedRate);
            stopBridge(child); activeRate = 0; state = SupervisorState::Idle;
            retryDelay = std::chrono::milliseconds(250); nextAction = now;
        }

        pollReady(child);

        int childStatus = 0;
        if (childExited(child, childStatus)) {
            const unsigned failedRate = activeRate;
            const auto runTime = childStarted == Clock::time_point::min() ? Clock::duration::zero() : now - childStarted;
            std::fprintf(stderr, "transport: native %u Hz engine exited", failedRate);
            if (WIFEXITED(childStatus)) std::fprintf(stderr, " with status %d", WEXITSTATUS(childStatus));
            else if (WIFSIGNALED(childStatus)) std::fprintf(stderr, " on signal %d", WTERMSIG(childStatus));
            std::fprintf(stderr, "\n");
            child = {}; activeRate = 0; childStarted = Clock::time_point::min();
            if (runTime >= kStableRun) retryDelay = std::chrono::milliseconds(250);

            if (!gStopRequested && failedRate != 0 && shared.rate() == failedRate) {
                const BootResult boot = tryGuardedBoot(bootHelper);
                switch (boot) {
                    case BootResult::CueIssued:
                        state = SupervisorState::WaitingForReenumeration; nextAction = now + kReenumerationDelay;
                        retryDelay = std::chrono::milliseconds(250);
                        std::printf("transport: guarded boot cue issued; state=%s\n", stateName(state)); break;
                    case BootResult::NoBootloader:
                        state = SupervisorState::Backoff; nextAction = now + retryDelay;
                        std::printf("transport: no FW410 bootloader present; state=%s retry=%lld ms\n", stateName(state), static_cast<long long>(retryDelay.count()));
                        retryDelay = std::min(retryDelay * 2, kMaxRetryDelay); break;
                    case BootResult::GuardRefused:
                        state = SupervisorState::Backoff; nextAction = now + std::chrono::seconds(2);
                        std::fprintf(stderr, "transport: FW410 bootloader candidate failed guard preflight; retrying conservatively\n"); break;
                    case BootResult::CandidateUnavailable:
                    case BootResult::Failed:
                        state = SupervisorState::Backoff; nextAction = now + retryDelay;
                        std::fprintf(stderr, "transport: bootloader check unavailable/failed; retry=%lld ms\n", static_cast<long long>(retryDelay.count()));
                        retryDelay = std::min(retryDelay * 2, kMaxRetryDelay); break;
                }
            } else state = SupervisorState::Idle;
        }

        if (child.pid <= 0 && requestedRate != 0 && now >= nextAction) {
            const std::string path = bridgePath(here, requestedRate);
            if (path.empty()) {
                std::fprintf(stderr, "transport: HAL selected unsupported rate %u Hz\n", requestedRate);
                state = SupervisorState::Backoff; nextAction = now + std::chrono::seconds(1);
            } else if (access(path.c_str(), X_OK) != 0) {
                std::fprintf(stderr, "transport: native %u Hz engine not built: %s\n", requestedRate, path.c_str());
                state = SupervisorState::Backoff; nextAction = now + std::chrono::seconds(1);
            } else {
                child = startBridge(path, requestedRate);
                if (child.pid > 0) { activeRate = requestedRate; state = SupervisorState::Running; childStarted = now; }
                else { state = SupervisorState::Backoff; nextAction = now + retryDelay; retryDelay = std::min(retryDelay * 2, kMaxRetryDelay); }
            }
        }

        pollReady(child);
        if (child.pid > 0 && childStarted != Clock::time_point::min() && now - childStarted >= kStableRun)
            retryDelay = std::chrono::milliseconds(250);

        macfw::hal::transport::State publishedState = macfw::hal::transport::State::Offline;
        if (requestedRate != 0)
            publishedState = (child.pid > 0 && child.ready)
                ? macfw::hal::transport::State::Online
                : macfw::hal::transport::State::Recovering;
        transportStatus.publish(publishedState, requestedRate, activeRate,
                                child.pid > 0 ? static_cast<std::uint32_t>(child.pid) : 0u);
        usleep(100000);
    }

    if (child.pid > 0) { std::printf("transport: stop requested; stopping native %u Hz engine\n", activeRate); stopBridge(child); }
    transportStatus.publish(macfw::hal::transport::State::Offline, shared.rate(), 0, 0);
    return 0;
}
