#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr const char* kSocketPath = "/tmp/macfw-fw410-control.sock";
constexpr std::array<const char*, 5> kHeadphoneMixerLabels = {
    "1/2", "3/4", "5/6", "7/8", "9/10"
};
constexpr std::array<const char*, 5> kOutputLabels = {
    "Analog 1/2", "Analog 3/4", "Analog 5/6", "Analog 7/8", "S/PDIF L/R"
};

int usage() {
    std::cerr
        << "usage:\n"
        << "  fw410ctl output-state get\n"
        << "  fw410ctl headphone-source get\n"
        << "  fw410ctl headphone-source set mixer|aux\n"
        << "  fw410ctl headphone-volume get\n"
        << "  fw410ctl headphone-volume set <dB|-inf> [<right-dB|-inf>]\n"
        << "  fw410ctl headphone-mixer get\n"
        << "  fw410ctl headphone-mixer set 1/2|3/4|5/6|7/8|9/10 on|off\n"
        << "  fw410ctl aux-stream12-volume get\n"
        << "  fw410ctl aux-stream12-volume set <dB|-inf> [<right-dB|-inf>]\n"
        << "  fw410ctl aux-output-volume get\n"
        << "  fw410ctl aux-output-volume set <dB|-inf> [<right-dB|-inf>]\n"
        << "\n"
        << "volume range: -128..0 dB in 1 dB steps; -inf uses AV/C negative infinity\n";
    return 64;
}

bool transact(const std::string& command, std::string& response) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "fw410ctl: socket: " << std::strerror(errno) << '\n';
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, kSocketPath, sizeof(address.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "fw410ctl: cannot connect to " << kSocketPath << ": "
                  << std::strerror(errno) << '\n';
        close(fd);
        return false;
    }

    const std::string wire = command + "\n";
    const char* p = wire.data();
    std::size_t left = wire.size();
    while (left > 0) {
        const ssize_t n = send(fd, p, left, 0);
        if (n <= 0) {
            std::cerr << "fw410ctl: send failed: " << std::strerror(errno) << '\n';
            close(fd);
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }

    response.clear();
    char buffer[256];
    while (response.find('\n') == std::string::npos) {
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            response.append(buffer, static_cast<std::size_t>(n));
            if (response.size() > 1024) break;
        } else {
            break;
        }
    }
    close(fd);
    return !response.empty();
}

bool getPayload(const std::string& command, std::string& payload) {
    std::string response;
    if (!transact(command, response)) return false;
    if (!response.empty() && response.back() == '\n') response.pop_back();
    if (response.rfind("OK ", 0) != 0) {
        std::cerr << "fw410ctl: " << response << '\n';
        return false;
    }
    payload = response.substr(3);
    return true;
}

bool dbToRaw(const std::string& text, int& raw) {
    if (text == "-inf" || text == "mute") {
        raw = -32768;
        return true;
    }
    char* end = nullptr;
    errno = 0;
    const long db = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || db < -128 || db > 0) return false;
    raw = static_cast<int>(db * 0x100);
    return true;
}

std::string rawToDb(int raw) {
    if (raw == -32768) return "-inf";
    if ((raw % 0x100) == 0)
        return std::to_string(raw / 0x100) + " dB";
    std::ostringstream out;
    out << raw << " raw (" << (static_cast<double>(raw) / 256.0) << " dB)";
    return out.str();
}

std::string outputSourceName(int source) {
    if (source == 0) return "mixer";
    if (source == 1) return "aux";
    return "unknown";
}

std::string spdifConnectorName(int source) {
    if (source == 0) return "coaxial";
    if (source == 1) return "optical";
    return "unknown";
}

int printOutputState() {
    std::cout << "FW410 physical output state (read-only):\n";
    for (std::size_t i = 0; i < kOutputLabels.size(); ++i) {
        std::string payload;
        if (!getPayload("OUTPUT_PAIR GET " + std::to_string(i), payload)) return 1;

        std::istringstream input(payload);
        int source = -1;
        int left = 0;
        int right = 0;
        std::string extra;
        if (!(input >> source >> left >> right) || (input >> extra)) {
            std::cerr << "fw410ctl: invalid output-state response: " << payload << '\n';
            return 1;
        }

        std::cout << "  " << kOutputLabels[i] << ":\n"
                  << "    source: " << outputSourceName(source) << " (" << source << ")\n"
                  << "    left:   " << rawToDb(left) << " (raw " << left << ")\n"
                  << "    right:  " << rawToDb(right) << " (raw " << right << ")\n";
    }

    std::string payload;
    if (!getPayload("SPDIF_CONNECTOR GET", payload)) return 1;
    std::istringstream input(payload);
    int connector = -1;
    std::string extra;
    if (!(input >> connector) || (input >> extra)) {
        std::cerr << "fw410ctl: invalid S/PDIF connector response: " << payload << '\n';
        return 1;
    }
    std::cout << "  S/PDIF connector: " << spdifConnectorName(connector)
              << " (" << connector << ")\n";
    return 0;
}

bool parseStereoRawResponse(const std::string& payload, int& left, int& right) {
    std::istringstream input(payload);
    std::string extra;
    return static_cast<bool>(input >> left >> right) && !(input >> extra);
}

int printLevelResponse(const std::string& payload) {
    int left = 0;
    int right = 0;
    if (!parseStereoRawResponse(payload, left, right)) {
        std::cout << payload << '\n';
        return 0;
    }
    std::cout << "left:  " << rawToDb(left) << " (raw " << left << ")\n"
              << "right: " << rawToDb(right) << " (raw " << right << ")\n";
    return 0;
}

bool buildVolumeCommand(const std::string& wireName,
                        int argc,
                        char** argv,
                        std::string& command) {
    const std::string action = argv[2];
    if (action == "get" && argc == 3) {
        command = wireName + " GET";
        return true;
    }
    if (action != "set" || (argc != 4 && argc != 5)) return false;

    int left = 0;
    int right = 0;
    if (!dbToRaw(argv[3], left)) return false;
    if (argc == 5) {
        if (!dbToRaw(argv[4], right)) return false;
    } else {
        right = left;
    }
    command = wireName + " SET " + std::to_string(left) + " " + std::to_string(right);
    return true;
}

int headphoneMixerIndex(const std::string& label) {
    for (std::size_t i = 0; i < kHeadphoneMixerLabels.size(); ++i)
        if (label == kHeadphoneMixerLabels[i]) return static_cast<int>(i);
    return -1;
}

int printHeadphoneMixer(const std::string& payload) {
    std::istringstream input(payload);
    for (const char* label : kHeadphoneMixerLabels) {
        int value = -1;
        if (!(input >> value) || (value != 0 && value != 1)) {
            std::cout << payload << '\n';
            return 0;
        }
        std::cout << label << ": " << (value ? "on" : "off") << '\n';
    }
    std::string extra;
    if (input >> extra) std::cout << "extra: " << extra << '\n';
    return 0;
}
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();

    const std::string control = argv[1];
    const std::string action = argv[2];

    if (control == "output-state") {
        if (action == "get" && argc == 3) return printOutputState();
        return usage();
    }

    std::string command;
    std::string headphoneMixerSetDisplay;
    bool levelResponse = false;
    bool sourceResponse = false;
    bool headphoneMixerResponse = false;

    if (control == "headphone-source") {
        if (action == "get" && argc == 3) {
            command = "HEADPHONE_SOURCE GET";
            sourceResponse = true;
        } else if (action == "set" && argc == 4) {
            std::string value = argv[3];
            if (value == "mixer") value = "0";
            else if (value == "aux") value = "1";
            if (value != "0" && value != "1") return usage();
            command = "HEADPHONE_SOURCE SET " + value;
            sourceResponse = true;
        } else {
            return usage();
        }
    } else if (control == "headphone-volume") {
        if (!buildVolumeCommand("HEADPHONE_VOLUME", argc, argv, command)) return usage();
        levelResponse = true;
    } else if (control == "headphone-mixer") {
        if (action == "get" && argc == 3) {
            command = "HEADPHONE_MIXER GET";
            headphoneMixerResponse = true;
        } else if (action == "set" && argc == 5) {
            const int index = headphoneMixerIndex(argv[3]);
            const std::string state = argv[4];
            if (index < 0 || (state != "on" && state != "off")) return usage();
            command = "HEADPHONE_MIXER SET " + std::to_string(index) +
                      " " + (state == "on" ? "1" : "0");
            headphoneMixerSetDisplay = std::string(argv[3]) + ": " + state;
        } else {
            return usage();
        }
    } else if (control == "aux-stream12-volume") {
        if (!buildVolumeCommand("AUX_STREAM12_VOLUME", argc, argv, command)) return usage();
        levelResponse = true;
    } else if (control == "aux-output-volume") {
        if (!buildVolumeCommand("AUX_OUTPUT_VOLUME", argc, argv, command)) return usage();
        levelResponse = true;
    } else {
        return usage();
    }

    std::string response;
    if (!transact(command, response)) return 1;
    if (!response.empty() && response.back() == '\n') response.pop_back();

    if (response.rfind("OK ", 0) == 0) {
        const std::string payload = response.substr(3);
        if (levelResponse) return printLevelResponse(payload);
        if (sourceResponse) {
            if (payload == "0") std::cout << "mixer (0)\n";
            else if (payload == "1") std::cout << "aux (1)\n";
            else std::cout << payload << '\n';
            return 0;
        }
        if (headphoneMixerResponse) return printHeadphoneMixer(payload);
        if (!headphoneMixerSetDisplay.empty()) {
            std::cout << headphoneMixerSetDisplay << '\n';
            return 0;
        }
        std::cout << payload << '\n';
        return 0;
    }

    std::cerr << "fw410ctl: " << response << '\n';
    return 1;
}
