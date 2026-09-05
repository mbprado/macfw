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

    // Hardware validation found that an FW410 left at 44.1 kHz can sometimes
    // retain a bad internal streaming/clock state across a clean transport
    // restart even though AV/C STATUS and all host-side transport counters look
    // healthy.  A real sample-rate transition reliably clears that state.
    // Re-arm 44.1 kHz through the already-proven guarded rate-control path
    // before establishing CMP/ISO.  Keep the clean-stop policy below unchanged.
    std::printf("initial FW410 44.1 kHz re-arm: 48000 -> 44100 Hz\n");
    if (runRateProbe(rateProbe, "48000") != 0) {
        std::fprintf(stderr, "initial 48000 Hz re-arm step failed\n");
        return 1;
    }
    if (runRateProbe(rateProbe, "44100") != 0) {
        std::fprintf(stderr, "initial 44100 Hz rate setup failed\n");
        return 1;
    }

    const int result = halbridge44100_inner_main();

    // A clean supervisor stop leaves the operational FW410 at the selected
    // native rate. The next 44.1 kHz start performs an explicit 48 -> 44.1
    // re-arm before ISO startup to clear any retained device streaming state.
    // Preserve the historical best-effort 48 kHz restore after an actual
    // engine/runtime failure for recovery.
    if (result == 0) {
        std::printf("clean 44.1 Hz stop: leaving FW410 at 44100 Hz\n");
        return 0;
    }

    std::printf("failed 44.1 Hz engine: restoring FW410 rate: 48000 Hz\n");
    const int restore = runRateProbe(rateProbe, "48000");
    if (restore != 0) std::fprintf(stderr, "warning: 48000 Hz restore failed\n");

    return result != 0 ? result : (restore == 0 ? 0 : 1);
}
