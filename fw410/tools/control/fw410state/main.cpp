#include <array>
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

constexpr std::array<const char*, 5> kOutputPairs = {"1/2", "3/4", "5/6", "7/8", "spdif"};
constexpr std::array<const char*, 5> kHeadphoneMixerPairs = {"1/2", "3/4", "5/6", "7/8", "9/10"};
constexpr std::array<const char*, 7> kMixerSources = {
    "analog", "spdif-in", "sw1/2", "sw3/4", "sw5/6", "sw7/8", "sw9/10"
};
constexpr std::array<const char*, 5> kMixerBuses = {"1/2", "3/4", "5/6", "7/8", "spdif"};

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

    std::fprintf(file, "# macfw FW410 persistent control state\n");
    std::fprintf(file, "# Updated after successful fw410ctl writes and replayed after transport startup/recovery.\n");
    std::fprintf(file, "# Do not edit while the control panel is changing settings.\n");
    for (const auto& entry : entries) {
        std::fprintf(file, "%s\t%s", kFormat, entry.key.c_str());
        for (const auto& arg : entry.args) std::fprintf(file, "\t%s", arg.c_str());
        std::fputc('\n', file);
    }

    bool ok = std::fflush(file) == 0;
    if (ok) ok = fsync(fd) == 0;
    if (std::fclose(file) != 0) ok = false;
    if (!ok) std::fprintf(stderr, "fw410state: failed writing %s\n", kStatePath);
    if (chmod(kStatePath, 0666) != 0)
        std::fprintf(stderr, "fw410state: warning: could not set state-file permissions: %s\n", std::strerror(errno));
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
        // Replayed writes must not recursively update the persistent file.
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

bool isMainMixerEntry(const Entry& entry) {
    return entry.key.rfind("mixer-route:", 0) == 0;
}

int restoreEntries(const char* argv0, const std::vector<Entry>& entries) {
    if (entries.empty()) {
        std::printf("fw410state: no saved control state\n");
        return 0;
    }

    const std::string tool = fw410ctlPath(argv0);
    if (access(tool.c_str(), X_OK) != 0) {
        std::fprintf(stderr, "fw410state: fw410ctl unavailable: %s\n", tool.c_str());
        return 127;
    }

    unsigned failures = 0;
    auto replay = [&](bool mixerPass) {
        for (const auto& entry : entries) {
            if (isMainMixerEntry(entry) != mixerPass) continue;
            const int rc = runControl(tool, entry);
            if (rc != 0) {
                ++failures;
                std::fprintf(stderr, "fw410state: restore failed for %s (status %d)\n", entry.key.c_str(), rc);
            }
        }
    };

    // Main-mixer routes go first. The first MAIN_MIXER write establishes the
    // validated complete 35-cell baseline before any differential route writes.
    replay(true);
    replay(false);

    if (failures) return 1;
    std::printf("fw410state: restored %zu saved controls\n", entries.size());
    return 0;
}

Entry makeEntry(std::string key, std::initializer_list<const char*> args) {
    Entry entry;
    entry.key = std::move(key);
    for (const char* arg : args) entry.args.emplace_back(arg);
    return entry;
}

std::vector<Entry> defaultState() {
    std::vector<Entry> entries;
    entries.reserve(35 + 5 + 5 + 1 + 1 + 5 + 2);

    // Validated macfw main-mixer baseline. Raw FW410 software-return identities
    // are rotated relative to CoreAudio order; keep the known safe AV/C mapping.
    for (const char* source : kMixerSources) {
        for (const char* bus : kMixerBuses) {
            bool enabled = false;
            const std::string src(source), dst(bus);
            enabled = (src == "sw3/4"  && dst == "1/2") ||
                      (src == "sw5/6"  && dst == "3/4") ||
                      (src == "sw7/8"  && dst == "5/6") ||
                      (src == "sw9/10" && dst == "7/8") ||
                      (src == "sw1/2"  && dst == "spdif");
            Entry entry;
            entry.key = "mixer-route:" + src + ":" + dst;
            entry.args = {"mixer-route", "set", src, dst, enabled ? "on" : "off"};
            entries.push_back(std::move(entry));
        }
    }

    for (const char* pair : kOutputPairs) {
        Entry source;
        source.key = std::string("output-source:") + pair;
        source.args = {"output-source", "set", pair, "mixer"};
        entries.push_back(std::move(source));

        Entry level;
        level.key = std::string("output-volume:") + pair;
        level.args = {"output-volume", "set", pair, "0", "0"};
        entries.push_back(std::move(level));
    }

    entries.push_back(makeEntry("headphone-source", {"headphone-source", "set", "mixer"}));
    entries.push_back(makeEntry("headphone-volume", {"headphone-volume", "set", "0", "0"}));

    for (std::size_t i = 0; i < kHeadphoneMixerPairs.size(); ++i) {
        Entry entry;
        entry.key = std::string("headphone-mixer:") + kHeadphoneMixerPairs[i];
        entry.args = {"headphone-mixer", "set", kHeadphoneMixerPairs[i], i == 0 ? "on" : "off"};
        entries.push_back(std::move(entry));
    }

    entries.push_back(makeEntry("aux-stream12-volume", {"aux-stream12-volume", "set", "0", "0"}));
    entries.push_back(makeEntry("aux-output-volume", {"aux-output-volume", "set", "0", "0"}));
    return entries;
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage:\n"
        "  %s record <key> <fw410ctl arguments...>\n"
        "  %s restore\n"
        "  %s reset\n"
        "  %s clear\n"
        "  %s show\n", argv0, argv0, argv0, argv0, argv0);
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

    if (command == "restore") return restoreEntries(argv[0], loadState());

    if (command == "clear") {
        if (!saveState({})) return 1;
        std::printf("fw410state: saved overrides cleared; current hardware state was not changed\n");
        return 0;
    }

    if (command == "reset") {
        const auto defaults = defaultState();
        if (!saveState(defaults)) return 1;
        const int rc = restoreEntries(argv[0], defaults);
        if (rc == 0) std::printf("fw410state: macfw defaults applied and saved\n");
        return rc;
    }

    usage(argv[0]);
    return 2;
}