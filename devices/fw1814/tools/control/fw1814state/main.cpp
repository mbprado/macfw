#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <libgen.h>
#include <limits.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr const char* kStatePath =
    "/Library/Application Support/macfw/fw1814/control-state.conf";
constexpr const char* kFormat = "macfw-fw1814-control-state-v1";

constexpr std::array<const char*, 2> kMixerSources{{"sw1/2", "sw3/4"}};
constexpr std::array<const char*, 2> kMixerBuses{{"1/2", "3/4"}};
constexpr std::array<const char*, 2> kOutputPairs{{"1/2", "3/4"}};

struct Entry {
    std::string key;
    std::vector<std::string> arguments;
};

bool safeField(const std::string& value) {
    return !value.empty() && value.find('\t') == std::string::npos &&
           value.find('\n') == std::string::npos &&
           value.find('\r') == std::string::npos;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const auto position = line.find('\t', start);
        if (position == std::string::npos) {
            fields.emplace_back(line.substr(start));
            break;
        }
        fields.emplace_back(line.substr(start, position - start));
        start = position + 1;
    }
    return fields;
}

bool validStoredCommand(const Entry& entry) {
    if (entry.arguments.size() == 5 &&
        entry.arguments[0] == "mixer-route" &&
        entry.arguments[1] == "set" &&
        (entry.arguments[2] == "sw1/2" ||
         entry.arguments[2] == "sw3/4") &&
        (entry.arguments[3] == "1/2" || entry.arguments[3] == "3/4") &&
        (entry.arguments[4] == "on" || entry.arguments[4] == "off"))
        return entry.key == "mixer-route:" + entry.arguments[2] + ":" +
                                entry.arguments[3];

    if (entry.arguments.size() == 4 &&
        entry.arguments[0] == "output-source" &&
        entry.arguments[1] == "set" &&
        (entry.arguments[2] == "1/2" || entry.arguments[2] == "3/4") &&
        (entry.arguments[3] == "mixer" || entry.arguments[3] == "aux"))
        return entry.key == "output-source:" + entry.arguments[2];

    return false;
}

std::vector<Entry> loadState() {
    std::vector<Entry> entries;
    std::ifstream input(kStatePath);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto fields = splitTabs(line);
        if (fields.size() < 3 || fields[0] != kFormat ||
            !safeField(fields[1]))
            continue;

        Entry entry;
        entry.key = fields[1];
        bool valid = true;
        for (std::size_t index = 2; index < fields.size(); ++index) {
            if (!safeField(fields[index])) {
                valid = false;
                break;
            }
            entry.arguments.push_back(fields[index]);
        }
        if (valid && validStoredCommand(entry))
            entries.push_back(std::move(entry));
    }
    return entries;
}

bool saveState(const std::vector<Entry>& entries) {
    const int fd = open(kStatePath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        std::fprintf(stderr, "fw1814state: cannot open %s: %s\n",
                     kStatePath, std::strerror(errno));
        return false;
    }
    FILE* file = fdopen(fd, "w");
    if (!file) {
        std::fprintf(stderr, "fw1814state: fdopen failed: %s\n",
                     std::strerror(errno));
        close(fd);
        return false;
    }

    std::fprintf(file, "# macfw FW1814 persistent control state\n");
    std::fprintf(file,
                 "# Updated after successful fw1814ctl writes and replayed "
                 "after transport startup/recovery.\n");
    std::fprintf(file,
                 "# Only validated typed controls are accepted.\n");
    for (const auto& entry : entries) {
        std::fprintf(file, "%s\t%s", kFormat, entry.key.c_str());
        for (const auto& argument : entry.arguments)
            std::fprintf(file, "\t%s", argument.c_str());
        std::fputc('\n', file);
    }

    bool ok = std::fflush(file) == 0;
    if (ok) ok = fsync(fd) == 0;
    if (std::fclose(file) != 0) ok = false;
    if (!ok)
        std::fprintf(stderr, "fw1814state: failed writing %s\n", kStatePath);
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

std::string controlPath(const char* argv0) {
    const std::string directory = executableDirectory(argv0);
    const std::string installed = directory + "/fw1814ctl";
    if (access(installed.c_str(), X_OK) == 0) return installed;
    return directory + "/../fw1814ctl/fw1814ctl";
}

int runControl(const std::string& tool, const Entry& entry) {
    std::vector<char*> arguments;
    arguments.reserve(entry.arguments.size() + 2);
    arguments.push_back(const_cast<char*>(tool.c_str()));
    for (const auto& argument : entry.arguments)
        arguments.push_back(const_cast<char*>(argument.c_str()));
    arguments.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) return 126;
    if (pid == 0) {
        setenv("MACFW_STATE_RESTORE", "1", 1);
        execv(tool.c_str(), arguments.data());
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

int restoreEntries(const char* argv0, const std::vector<Entry>& entries) {
    if (entries.empty()) {
        std::printf("fw1814state: no saved control state\n");
        return 0;
    }

    const std::string tool = controlPath(argv0);
    if (access(tool.c_str(), X_OK) != 0) {
        std::fprintf(stderr, "fw1814state: fw1814ctl unavailable: %s\n",
                     tool.c_str());
        return 127;
    }

    unsigned failures = 0;
    for (const auto& entry : entries) {
        const int result = runControl(tool, entry);
        if (result != 0) {
            ++failures;
            std::fprintf(stderr,
                         "fw1814state: restore failed for %s (status %d)\n",
                         entry.key.c_str(), result);
        }
    }
    if (failures != 0) return 1;
    std::printf("fw1814state: restored %zu saved controls\n", entries.size());
    return 0;
}

std::vector<Entry> defaultState() {
    std::vector<Entry> entries;
    entries.reserve(6);
    for (const char* sourceValue : kMixerSources) {
        for (const char* busValue : kMixerBuses) {
            const std::string source(sourceValue);
            const std::string bus(busValue);
            const bool enabled =
                (source == "sw1/2" && bus == "1/2") ||
                (source == "sw3/4" && bus == "3/4");
            Entry entry;
            entry.key = "mixer-route:" + source + ":" + bus;
            entry.arguments = {
                "mixer-route", "set", source, bus,
                enabled ? "on" : "off",
            };
            entries.push_back(std::move(entry));
        }
    }
    for (const char* pairValue : kOutputPairs) {
        const std::string pair(pairValue);
        Entry entry;
        entry.key = "output-source:" + pair;
        entry.arguments = {"output-source", "set", pair, "mixer"};
        entries.push_back(std::move(entry));
    }
    return entries;
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s record <key> <fw1814ctl arguments...>\n"
                 "  %s restore\n"
                 "  %s reset\n"
                 "  %s clear\n"
                 "  %s show\n",
                 argv0, argv0, argv0, argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    const std::string command = argv[1];

    if (command == "record") {
        if (argc < 5 || !safeField(argv[2])) {
            usage(argv[0]);
            return 2;
        }
        Entry updated;
        updated.key = argv[2];
        for (int index = 3; index < argc; ++index) {
            if (!safeField(argv[index])) {
                std::fprintf(stderr, "fw1814state: invalid argument\n");
                return 2;
            }
            updated.arguments.emplace_back(argv[index]);
        }
        if (!validStoredCommand(updated)) {
            std::fprintf(stderr,
                         "fw1814state: unsupported control-state entry\n");
            return 2;
        }

        auto entries = loadState();
        bool replaced = false;
        for (auto& entry : entries) {
            if (entry.key == updated.key) {
                entry = updated;
                replaced = true;
                break;
            }
        }
        if (!replaced) entries.push_back(std::move(updated));
        return saveState(entries) ? 0 : 1;
    }

    if (command == "show") {
        for (const auto& entry : loadState()) {
            std::printf("%s:", entry.key.c_str());
            for (const auto& argument : entry.arguments)
                std::printf(" %s", argument.c_str());
            std::printf("\n");
        }
        return 0;
    }

    if (command == "restore")
        return restoreEntries(argv[0], loadState());

    if (command == "clear") {
        if (!saveState({})) return 1;
        std::printf("fw1814state: saved overrides cleared; current hardware "
                    "state was not changed\n");
        return 0;
    }

    if (command == "reset") {
        const auto defaults = defaultState();
        if (!saveState(defaults)) return 1;
        const int result = restoreEntries(argv[0], defaults);
        if (result == 0)
            std::printf("fw1814state: macfw defaults applied and saved\n");
        return result;
    }

    usage(argv[0]);
    return 2;
}
