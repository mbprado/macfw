#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <libgen.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

int coreaudiobridge44100_inner_main(int argc, char** argv);

namespace {

bool hasExecuteFlag(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--execute") == 0)
            return true;
    }
    return false;
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

int runRateProbe(const std::string& rateProbe, const char* rate) {
    const pid_t pid = fork();
    if (pid < 0) {
        std::fprintf(stderr, "fork rateprobe failed: %s\n", std::strerror(errno));
        return 127;
    }
    if (pid == 0) {
        execl(rateProbe.c_str(), rateProbe.c_str(), rate,
              "--execute", "--keep", static_cast<char*>(nullptr));
        std::fprintf(stderr, "exec %s failed: %s\n",
                     rateProbe.c_str(), std::strerror(errno));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        std::fprintf(stderr, "waitpid rateprobe failed: %s\n", std::strerror(errno));
        return 127;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128;
}

bool restore48000(const std::string& rateProbe) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (runRateProbe(rateProbe, "48000") == 0)
            return true;
        sleep(1);
    }
    std::fprintf(stderr, "warning: unable to restore FW410 to 48000 Hz\n");
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (!hasExecuteFlag(argc, argv))
        return coreaudiobridge44100_inner_main(argc, argv);

    const std::string here = executableDirectory(argc > 0 ? argv[0] : nullptr);
    const std::string rateProbe = here + "/../../control/rateprobe/rateprobe";

    std::printf("initial FW410 rate setup: 44100 Hz\n");
    if (runRateProbe(rateProbe, "44100") != 0) {
        std::fprintf(stderr, "status: FAIL - unable to set FW410 to 44100 Hz\n");
        return 1;
    }

    const int result = coreaudiobridge44100_inner_main(argc, argv);

    std::printf("restoring FW410 rate: 48000 Hz\n");
    const bool restored = restore48000(rateProbe);
    return result != 0 ? result : (restored ? 0 : 1);
}
