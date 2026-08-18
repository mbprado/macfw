#include "macfw_hal_shm.h"

#include <cerrno>
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

pid_t startBridge(const std::string& path, unsigned rate) {
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    std::printf("transport: started native %u Hz engine (pid %d)\n", rate, static_cast<int>(pid));
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
    unsigned activeRate = 0;
    pid_t child = -1;

    std::printf("macfw haltransport — rate-aware CoreAudio HAL to FW410 transport supervisor\n");
    std::printf("supported native rates: 44100, 48000 Hz\n");
    std::printf("change the device format in Audio MIDI Setup to switch engines\n");

    while (!gStopRequested) {
        const unsigned requestedRate = shared.rate();
        if (requestedRate != activeRate) {
            if (child > 0) {
                std::printf("transport: HAL rate changed %u -> %u Hz; stopping old engine\n",
                            activeRate, requestedRate);
                stopBridge(child);
                activeRate = 0;
            }

            const std::string path = bridgePath(here, requestedRate);
            if (!path.empty()) {
                if (access(path.c_str(), X_OK) != 0) {
                    std::fprintf(stderr, "transport: native %u Hz engine not built: %s\n",
                                 requestedRate, path.c_str());
                } else {
                    child = startBridge(path, requestedRate);
                    if (child > 0) activeRate = requestedRate;
                }
            } else if (requestedRate != 0) {
                std::fprintf(stderr, "transport: HAL selected unsupported rate %u Hz\n", requestedRate);
            }
        }

        int childStatus = 0;
        if (childExited(child, childStatus)) {
            std::fprintf(stderr, "transport: native %u Hz engine exited", activeRate);
            if (WIFEXITED(childStatus)) std::fprintf(stderr, " with status %d", WEXITSTATUS(childStatus));
            else if (WIFSIGNALED(childStatus)) std::fprintf(stderr, " on signal %d", WTERMSIG(childStatus));
            std::fprintf(stderr, "\n");
            child = -1;
            activeRate = 0;
        }

        usleep(100000);
    }

    if (child > 0) {
        std::printf("transport: stop requested; stopping native %u Hz engine\n", activeRate);
        stopBridge(child);
    }
    return 0;
}
