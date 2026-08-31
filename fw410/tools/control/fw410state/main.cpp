#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <libgen.h>
#include <limits.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr const char* kStatePath = "/Library/Application Support/macfw/fw410/control-state.conf";
constexpr const char* kFormat = "macfw-fw410-control-state-v1";

struct Entry {
    std::string key;
    std::vector<std::string> args;
};

bool safeField(const std::string& value) {
    return !value.empty() && value.find('\t') == std::string::npos &&
           value.find('\n') == std::string::npos && value.find('\r') == std::string::npos;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const auto pos = line.find('\t', start);
        if (pos == std::string::npos) {
            fields.emplace_back(line.substr(start));
            break;
        }
        fields.emplace_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return fields;
}

std::vector<Entry> loadState() {
    std::vector<Entry> entries;
    std::ifstream in(kStatePath);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto fields = splitTabs(line);
        if (fields.size() < 3 || fields[0] != kFormat || !safeField(fields[1])) continue;
        Entry entry;
        entry.key = fields[1];
        bool valid = true;
        for (std::size_t i = 2; i < fields.size(); ++i) {
            if (!safeField(fields[i])) { valid = false; break; }
            entry.args.push_back(fields[i]);
        }
        if (valid && !entry.args.empty()) entries.push_back(std::move(entry));
    }
    return entries;
}

bool saveState(const std::vector<Entry>& entries) {
    const int fd = open(kStatePath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        std::fprintf(stderr, "fw410state: cannot open %s: %s\n", kStatePath, std::strerror(errno));
        return false;
    }
    FILE* file = fdopen(fd, "w");
    if (!file) {
        std::fprintf(stderr, "fw410state: fdopen failed: %s\n", std::strerror(errno));
        close(fd);
        return false;
    }
    std::fprintf(file, "# macfw FW410 control panel state\n");
    std::fprintf(file, "# This file is updated by the control panel and replayed after transport startup/recovery.\n");
    for (const auto& entry : entries) {
        std::fprintf(file, "%s\t%s", kFormat, entry.key.c_str());
        for (const auto& arg : entry.args) std::fprintf(file, "\t%s", arg.c_str());
        std::fputc('\n', file);
    }
    const bool ok = std::fflush(file) == 0 && fsync(fd) == 0 && std::fclose(file) == 0;
    if (!ok) std::fprintf(stderr, "fw410state: failed writing %s\n", kStatePath);
    chmod(kStatePath, 0666);
    return ok;
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

std::string fw410ctlPath(const char* argv0) {
    return executableDirectory(argv0) + "/../fw410ctl/fw410ctl";
}

int runControl(const std::string& tool, const Entry& entry) {
    std::vector<char*> argv;
    argv.reserve(entry.args.size() + 2);
    argv.push_back(const_cast<char*>(tool.c_str()));
    for (const auto& arg : entry.args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) return 126;
    if (pid == 0) {
        setenv("MACFW_STATE_RESTORE", "1", 1);
        execv(tool.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return 126;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 126;
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage:\n"
        "  %s record <key> <fw410ctl arguments...>\n"
        "  %s restore\n"
        "  %s show\n", argv0, argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    const std::string command = argv[1];

    if (command == "record") {
        if (argc < 5 || !safeField(argv[2])) { usage(argv[0]); return 2; }
        Entry updated;
        updated.key = argv[2];
        for (int i = 3; i < argc; ++i) {
            if (!safeField(argv[i])) { std::fprintf(stderr, "fw410state: invalid argument\n"); return 2; }
            updated.args.emplace_back(argv[i]);
        }
        auto entries = loadState();
        bool replaced = false;
        for (auto& entry : entries) {
            if (entry.key == updated.key) { entry = updated; replaced = true; break; }
        }
        if (!replaced) entries.push_back(std::move(updated));
        return saveState(entries) ? 0 : 1;
    }

    if (command == "show") {
        for (const auto& entry : loadState()) {
            std::printf("%s:", entry.key.c_str());
            for (const auto& arg : entry.args) std::printf(" %s", arg.c_str());
            std::printf("\n");
        }
        return 0;
    }

    if (command == "restore") {
        const auto entries = loadState();
        if (entries.empty()) {
            std::printf("fw410state: no saved control state\n");
            return 0;
        }
        const std::string tool = fw410ctlPath(argv[0]);
        if (access(tool.c_str(), X_OK) != 0) {
            std::fprintf(stderr, "fw410state: fw410ctl unavailable: %s\n", tool.c_str());
            return 127;
        }
        unsigned failures = 0;
        for (const auto& entry : entries) {
            const int rc = runControl(tool, entry);
            if (rc != 0) {
                ++failures;
                std::fprintf(stderr, "fw410state: restore failed for %s (status %d)\n", entry.key.c_str(), rc);
            }
        }
        if (failures) return 1;
        std::printf("fw410state: restored %zu saved controls\n", entries.size());
        return 0;
    }

    usage(argv[0]);
    return 2;
}
