#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <libgen.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

int halbridge48000_inner_main();

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

bool alreadyAt48000(const std::string& path) {
    const std::string command = "\"" + path + "\"";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return false;

    bool output48 = false;
    bool input48 = false;
    char line[512] = {};
    while (std::fgets(line, sizeof(line), pipe)) {
        const std::string text(line);
        if (text.find("device OUTPUT / host capture:") != std::string::npos &&
            text.find("48000 Hz") != std::string::npos)
            output48 = true;
        if (text.find("device INPUT / host playback:") != std::string::npos &&
            text.find("48000 Hz") != std::string::npos)
            input48 = true;
    }
    const int status = pclose(pipe);
    return status == 0 && output48 && input48;
}
}

int main(int argc, char** argv) {
    const std::string here = executableDirectory(argc > 0 ? argv[0] : nullptr);
    const std::string rateProbe = here + "/../../control/rateprobe/rateprobe";

    if (alreadyAt48000(rateProbe)) {
        std::printf("FW410 rate setup: already 48000 Hz; no AV/C rate CONTROL sent\n");
    } else {
        std::printf("initial FW410 rate setup: 48000 Hz\n");
        if (runRateProbe(rateProbe, "48000") != 0) {
            std::fprintf(stderr, "initial 48000 Hz rate setup failed\n");
            return 1;
        }
    }

    return halbridge48000_inner_main();
}
