#include "../hal/include/macfw_fw1814_hal_shm.h"

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

bool halPlaybackReady() {
    const int fd = shm_open(macfw::fw1814::hal::kShmName, O_RDWR, 0);
    if (fd < 0) return false;

    void* p = mmap(nullptr, sizeof(macfw::fw1814::hal::SharedPcmRing),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;

    auto* ring = static_cast<macfw::fw1814::hal::SharedPcmRing*>(p);
    const bool ready = macfw::fw1814::hal::valid(*ring) &&
                       ring->sampleRate.load(std::memory_order_acquire) == 48000;
    munmap(p, sizeof(*ring));
    return ready;
}

int runChild(const std::string& path, const char* arg1, const char* arg2) {
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
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            if (gStopRequested) kill(pid, SIGTERM);
            continue;
        }
        return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const std::string here = executableDirectory(argc > 0 ? argv[0] : nullptr);
    const std::string initPath = here + "/fw1814init";
    const std::string enginePath = here + "/fw1814analog48";

    std::printf("macfw fw1814supervisor — fixed 48 kHz transport supervisor\n");
    std::printf("waiting for FW1814 HAL shared memory at 48000 Hz\n");
    std::fflush(stdout);

    while (!gStopRequested && !halPlaybackReady())
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

    if (gStopRequested) return 0;

    std::printf("FW1814 HAL shared memory ready; applying validated init-48\n");
    std::fflush(stdout);
    const int initStatus = runChild(initPath, "48000", "--execute");
    if (initStatus != 0) {
        std::fprintf(stderr, "FW1814 init-48 failed with status %d\n", initStatus);
        return initStatus;
    }

    std::printf("FW1814 init-48 PASS; starting analog transport engine\n");
    std::fflush(stdout);
    execl(enginePath.c_str(), enginePath.c_str(), static_cast<char*>(nullptr));
    std::fprintf(stderr, "FW1814 supervisor exec failed for %s: %s\n",
                 enginePath.c_str(), std::strerror(errno));
    return 127;
}
