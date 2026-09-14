#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <libgen.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

int halbridge44100_inner_main();

namespace {
std::string executableDirectory(const char* argv0) {
    char resolved[PATH_MAX] = {};
    if (argv0 && realpath(argv0, resolved)) {
        char copy[PATH_MAX] = {};
        std::strncpy(copy, resolved, sizeof(copy) - 1);
        return dirname(copy);
    }
    return ".";
}

int runRateProbe(const std::string& path, const char* rate) {
    const pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        execl(path.c_str(), path.c_str(), rate, "--execute", "--keep", static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return 127;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
}

int main(int argc, char** argv) {
    const std::string here = executableDirectory(argc > 0 ? argv[0] : nullptr);
    const std::string rateProbe = here + "/../../control/rateprobe/rateprobe";

    std::printf("initial FW410 rate setup: 44100 Hz\n");
    if (runRateProbe(rateProbe, "44100") != 0) {
        std::fprintf(stderr, "initial 44100 Hz rate setup failed\n");
        return 1;
    }

    const int result = halbridge44100_inner_main();

    // A clean supervisor stop leaves the operational FW410 at the selected
    // native rate. This avoids an unnecessary 44.1 -> 48 -> 44.1 transition
    // across normal 44.1 kHz restarts. Preserve the historical best-effort
    // 48 kHz restore after an actual engine/runtime failure for recovery.
    if (result == 0) {
        std::printf("clean 44.1 Hz stop: leaving FW410 at 44100 Hz\n");
        return 0;
    }

    std::printf("failed 44.1 Hz engine: restoring FW410 rate: 48000 Hz\n");
    const int restore = runRateProbe(rateProbe, "48000");
    if (restore != 0) std::fprintf(stderr, "warning: 48000 Hz restore failed\n");

    return result != 0 ? result : (restore == 0 ? 0 : 1);
}
