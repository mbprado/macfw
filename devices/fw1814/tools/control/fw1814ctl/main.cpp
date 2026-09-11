#include "../../../special_mixer_model.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <libgen.h>
#include <limits.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using RoutingModel = macfw::fw1814::SpecialMixerRoutingModel;

constexpr const char* kSocketPath = "/tmp/macfw-fw1814-control.sock";
constexpr std::array<const char*, RoutingModel::kStreamSourceCount>
    kMixerSourceArgs{{"sw1/2", "sw3/4"}};
constexpr std::array<const char*, RoutingModel::kStreamSourceCount>
    kMixerSourceLabels{{"SW Return 1/2", "SW Return 3/4"}};
constexpr std::array<const char*, RoutingModel::kMixerBusCount>
    kMixerBusArgs{{"1/2", "3/4"}};
constexpr std::array<const char*, RoutingModel::kAnalogInputPairCount>
    kInputPairArgs{{"analog1/2", "analog3/4", "analog5/6", "analog7/8"}};
constexpr std::array<const char*, RoutingModel::kAnalogInputPairCount>
    kInputPairLabels{{
        "Analog Inputs 1/2",
        "Analog Inputs 3/4",
        "Analog Inputs 5/6",
        "Analog Inputs 7/8",
    }};
constexpr std::array<const char*, RoutingModel::kAnalogOutputPairCount>
    kOutputPairArgs{{"1/2", "3/4"}};
constexpr std::array<const char*, RoutingModel::kAnalogOutputPairCount>
    kOutputPairLabels{{"Analog Outputs 1/2", "Analog Outputs 3/4"}};
constexpr std::array<const char*, 3> kHeadphoneSourceArgs{{
    "mixer1/2", "mixer3/4", "aux",
}};
constexpr std::array<const char*, 3> kHeadphoneSourceLabels{{
    "Mixer 1/2", "Mixer 3/4", "AUX 1/2",
}};
constexpr std::array<const char*, RoutingModel::kHeadphoneOutputCount>
    kHeadphoneOutputArgs{{"1", "2"}};
constexpr std::array<const char*, RoutingModel::kHeadphoneOutputCount>
    kHeadphoneOutputLabels{{"Headphone Output 1", "Headphone Output 2"}};

int usage() {
    std::cerr
        << "usage:\n"
        << "  fw1814ctl routing get\n"
        << "  fw1814ctl mixer get\n"
        << "  fw1814ctl mixer-route get sw1/2|sw3/4 1/2|3/4\n"
        << "  fw1814ctl mixer-route set sw1/2|sw3/4 1/2|3/4 on|off\n"
        << "  fw1814ctl software-return-level get sw1/2|sw3/4\n"
        << "  fw1814ctl software-return-level set-all "
           "sw1/2|sw3/4 mute|unity\n"
        << "  fw1814ctl software-return-level set sw1/2|sw3/4 "
           "<dB|-inf> [<right-dB|-inf>]\n"
        << "  fw1814ctl input-mixer get\n"
        << "  fw1814ctl input-mixer-route get "
           "analog1/2|analog3/4|analog5/6|analog7/8 1/2|3/4\n"
        << "  fw1814ctl input-mixer-route set "
           "analog1/2|analog3/4|analog5/6|analog7/8 1/2|3/4 on|off\n"
        << "  fw1814ctl input-monitor-level get "
           "analog1/2|analog3/4|analog5/6|analog7/8\n"
        << "  fw1814ctl input-monitor-level set-all "
           "analog1/2|analog3/4|analog5/6|analog7/8 mute|unity\n"
        << "  fw1814ctl input-monitor-level set-all "
           "analog1/2|analog3/4|analog5/6|analog7/8 -20db\n"
        << "  fw1814ctl input-monitor-level set "
           "analog1/2|analog3/4|analog5/6|analog7/8 "
           "<dB|-inf> [<right-dB|-inf>]\n"
        << "  fw1814ctl input-monitor-channel-level get "
           "analog1/2|analog3/4|analog5/6|analog7/8 left|right"
           "  # diagnostic\n"
        << "  fw1814ctl input-monitor-channel-level set "
           "analog1/2|analog3/4|analog5/6|analog7/8 left|right "
           "unity|-20db  # diagnostic\n"
        << "  fw1814ctl input-monitor-pan get "
           "analog1/2|analog3/4|analog5/6|analog7/8\n"
        << "  fw1814ctl input-monitor-pan set "
           "analog1/2|analog3/4|analog5/6|analog7/8 left|right "
           "left|center|right\n"
        << "  fw1814ctl input-monitor-pan set-percent "
           "analog1/2|analog3/4|analog5/6|analog7/8 left|right "
           "-100..100\n"
        << "  fw1814ctl output-state get\n"
        << "  fw1814ctl output-source get 1/2|3/4\n"
        << "  fw1814ctl output-source set 1/2|3/4 mixer|aux\n"
        << "  fw1814ctl headphone-state get\n"
        << "  fw1814ctl headphone-source get 1|2\n"
        << "  fw1814ctl headphone-source set 1|2 mixer1/2|mixer3/4\n"
        << "  fw1814ctl capabilities get\n"
        << "  fw1814ctl engine get\n\n"
        << "FW1814 mixer registers are write-only. The active transport "
           "establishes a known startup baseline and maintains the "
           "authoritative routing cache. Successful changes to validated "
           "controls are saved and restored after engine startup.\n"
        << "Monitor-level range: -128..0 dB in 1 dB steps; -inf uses "
           "AV/C negative infinity.\n";
    return 64;
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

std::string stateHelperPath(const char* argv0) {
    const std::string directory = executableDirectory(argv0);
    const std::string installed = directory + "/fw1814state";
    if (access(installed.c_str(), X_OK) == 0) return installed;
    return directory + "/../fw1814state/fw1814state";
}

void persistSuccessfulSet(const char* argv0,
                          const std::string& key,
                          int argc,
                          char** argv) {
    if (std::getenv("MACFW_STATE_RESTORE")) return;
    const std::string helper = stateHelperPath(argv0);
    if (access(helper.c_str(), X_OK) != 0) {
        std::cerr << "fw1814ctl: warning: state helper unavailable; setting "
                     "was not persisted\n";
        return;
    }

    std::vector<char*> arguments;
    arguments.reserve(static_cast<std::size_t>(argc) + 3);
    arguments.push_back(const_cast<char*>(helper.c_str()));
    arguments.push_back(const_cast<char*>("record"));
    arguments.push_back(const_cast<char*>(key.c_str()));
    for (int index = 1; index < argc; ++index)
        arguments.push_back(argv[index]);
    arguments.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "fw1814ctl: warning: could not start state helper\n";
        return;
    }
    if (pid == 0) {
        execv(helper.c_str(), arguments.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        std::cerr << "fw1814ctl: warning: state helper wait failed\n";
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        std::cerr << "fw1814ctl: warning: setting worked but could not be "
                     "persisted\n";
}

bool transact(const std::string& command, std::string& response) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "fw1814ctl: socket: " << std::strerror(errno) << '\n';
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, kSocketPath,
                 sizeof(address.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != 0) {
        std::cerr << "fw1814ctl: cannot connect to " << kSocketPath << ": "
                  << std::strerror(errno) << '\n';
        close(fd);
        return false;
    }

    const bool restoring = std::getenv("MACFW_STATE_RESTORE") != nullptr;
    const std::string request =
        (restoring && command != "CONTROL READY" ? "RESTORE " : "") +
        command + "\n";
    const char* cursor = request.data();
    std::size_t remaining = request.size();
    while (remaining != 0) {
        const ssize_t count = send(fd, cursor, remaining, 0);
        if (count <= 0) {
            std::cerr << "fw1814ctl: send failed\n";
            close(fd);
            return false;
        }
        cursor += count;
        remaining -= static_cast<std::size_t>(count);
    }

    response.clear();
    char buffer[256];
    while (response.find('\n') == std::string::npos) {
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        response.append(buffer, static_cast<std::size_t>(count));
        if (response.size() > 2048) break;
    }
    close(fd);
    return !response.empty();
}

bool payloadFor(const std::string& command, std::string& payload) {
    std::string response;
    if (!transact(command, response)) return false;
    if (!response.empty() && response.back() == '\n') response.pop_back();
    if (response.rfind("OK ", 0) != 0) {
        std::cerr << "fw1814ctl: " << response << '\n';
        return false;
    }
    payload = response.substr(3);
    return true;
}

bool parseRawWord(const std::string& text, std::uint32_t& value) {
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 0);
    if (errno != 0 || !end || *end != '\0' || parsed > 0xfffffffful)
        return false;
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parseSignedRange(const std::string& text,
                      int minimum,
                      int maximum,
                      int& value) {
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || !end || end == text.c_str() || *end != '\0' ||
        parsed < minimum || parsed > maximum)
        return false;
    value = static_cast<int>(parsed);
    return true;
}

bool dbToRaw(const std::string& text, int& raw) {
    if (text == "-inf" || text == "mute") {
        raw = -32768;
        return true;
    }
    int db = 0;
    if (!parseSignedRange(text, -128, 0, db)) return false;
    raw = db * 0x100;
    return true;
}

std::string rawToDb(int raw) {
    if (raw == -32768) return "-inf (mute)";
    if (raw % 0x100 == 0)
        return std::to_string(raw / 0x100) + " dB";
    std::ostringstream output;
    output << raw << " raw (" << (static_cast<double>(raw) / 256.0)
           << " dB)";
    return output.str();
}

int indexOf(const std::string& value,
            const char* const* choices,
            std::size_t count) {
    for (std::size_t index = 0; index < count; ++index)
        if (value == choices[index]) return static_cast<int>(index);
    return -1;
}

const char* onOff(bool value) { return value ? "on" : "off"; }
const char* outputSourceName(unsigned value) {
    return value == 0 ? "mixer" : value == 1 ? "aux" : "unknown";
}

const char* headphoneSourceName(unsigned value) {
    return value < kHeadphoneSourceLabels.size()
        ? kHeadphoneSourceLabels[value]
        : "unknown";
}

int routingGet() {
    std::string payload;
    if (!payloadFor("ROUTING GET", payload)) return 1;

    std::istringstream input(payload);
    std::string profile;
    unsigned rate = 0;
    std::string mixStreamText;
    std::string analogOutputText;
    std::string headphoneOutputText;
    std::string inputMixerText;
    std::string stateSource;
    std::string extra;
    if (!(input >> profile >> rate >> mixStreamText >> analogOutputText >>
          headphoneOutputText >> inputMixerText >> stateSource) ||
        (input >> extra)) {
        std::cerr << "fw1814ctl: invalid routing response: " << payload
                  << '\n';
        return 1;
    }

    std::uint32_t mixStreamIn = 0;
    std::uint32_t srcAnalogOut = 0;
    std::uint32_t srcHeadphoneOut = 0;
    std::uint32_t mixAnalogDigitalIn = 0;
    if (!parseRawWord(mixStreamText, mixStreamIn) ||
        !parseRawWord(analogOutputText, srcAnalogOut) ||
        !parseRawWord(headphoneOutputText, srcHeadphoneOut) ||
        !parseRawWord(inputMixerText, mixAnalogDigitalIn) ||
        mixAnalogDigitalIn > 0xffu) {
        std::cerr << "fw1814ctl: invalid routing register value\n";
        return 1;
    }

    std::cout << "FW1814 routing state (active engine cache):\n"
              << "  profile: " << profile << '\n'
              << "  sample rate: " << rate << " Hz\n";
    for (std::size_t source = 0;
         source < RoutingModel::kStreamSourceCount; ++source) {
        std::cout << "  " << kMixerSourceLabels[source] << ":";
        for (std::size_t destination = 0;
             destination < RoutingModel::kMixerBusCount; ++destination) {
            const bool enabled =
                (mixStreamIn & RoutingModel::kStreamRouteMasks[source]
                                                          [destination]) != 0;
            std::cout << "  " << kMixerBusArgs[destination] << '='
                      << onOff(enabled);
        }
        std::cout << '\n';
    }
    for (std::size_t source = 0;
         source < RoutingModel::kAnalogInputPairCount; ++source) {
        std::cout << "  " << kInputPairLabels[source] << ":";
        for (std::size_t destination = 0;
             destination < RoutingModel::kMixerBusCount; ++destination) {
            const bool enabled =
                (mixAnalogDigitalIn &
                 RoutingModel::kAnalogInputRouteMasks[source][destination]) !=
                0;
            std::cout << "  " << kMixerBusArgs[destination] << '='
                      << onOff(enabled);
        }
        std::cout << '\n';
    }
    for (std::size_t pair = 0;
         pair < RoutingModel::kAnalogOutputPairCount; ++pair) {
        const unsigned source = (srcAnalogOut >> pair) & 1u;
        std::cout << "  " << kOutputPairLabels[pair] << ": "
                  << outputSourceName(source) << '\n';
    }
    for (std::size_t output = 0;
         output < RoutingModel::kHeadphoneOutputCount; ++output) {
        const std::uint32_t field =
            (srcHeadphoneOut >> (output * 16)) & 0xffffu;
        const unsigned source =
            field & 0x04u ? 2u : field & 0x02u ? 1u : 0u;
        std::cout << "  " << kHeadphoneOutputLabels[output] << ": "
                  << headphoneSourceName(source) << '\n';
    }
    std::cout << "  MIX_STM_IN: " << mixStreamText << '\n'
              << "  SRC_ANA_OUT: " << analogOutputText << '\n'
              << "  SRC_HP_OUT: " << headphoneOutputText << '\n'
              << "  MIX_ANA_DIG_IN: " << inputMixerText << '\n'
              << "  hardware readback: unavailable (" << stateSource
              << ")\n";
    return 0;
}

int mixerGet() {
    std::string payload;
    if (!payloadFor("MIXER GET", payload)) return 1;
    std::istringstream input(payload);
    std::array<std::array<unsigned, RoutingModel::kMixerBusCount>,
               RoutingModel::kStreamSourceCount> routes{};
    for (auto& source : routes)
        for (auto& destination : source)
            if (!(input >> destination) || destination > 1) {
                std::cerr << "fw1814ctl: invalid mixer response: " << payload
                          << '\n';
                return 1;
            }
    std::string raw;
    std::string extra;
    if (!(input >> raw) || (input >> extra)) {
        std::cerr << "fw1814ctl: invalid mixer response: " << payload << '\n';
        return 1;
    }

    std::cout << "FW1814 software-return mixer (active engine cache):\n";
    for (std::size_t source = 0; source < routes.size(); ++source) {
        std::cout << "  " << kMixerSourceLabels[source] << ":";
        for (std::size_t destination = 0;
             destination < routes[source].size(); ++destination)
            std::cout << "  " << kMixerBusArgs[destination] << '='
                      << onOff(routes[source][destination] != 0);
        std::cout << '\n';
    }
    std::cout << "  MIX_STM_IN: " << raw << " (write-only cache)\n";
    return 0;
}

int mixerRouteCommand(const std::string& action, int argc, char** argv) {
    if ((action == "get" && argc != 5) ||
        (action == "set" && argc != 6))
        return usage();
    const int source = indexOf(argv[3], kMixerSourceArgs.data(),
                               kMixerSourceArgs.size());
    const int destination = indexOf(argv[4], kMixerBusArgs.data(),
                                    kMixerBusArgs.size());
    if (source < 0 || destination < 0) return usage();

    int value = -1;
    if (action == "set") {
        const std::string state = argv[5];
        value = state == "on" ? 1 : state == "off" ? 0 : -1;
        if (value < 0) return usage();
    }

    std::string command = "MIXER ROUTE " +
        std::string(action == "get" ? "GET " : "SET ") +
        std::to_string(source) + " " + std::to_string(destination);
    if (action == "set") command += " " + std::to_string(value);

    std::string payload;
    if (!payloadFor(command, payload)) return 1;
    std::istringstream input(payload);
    int returnedSource = -1;
    int returnedDestination = -1;
    int returnedValue = -1;
    std::string extra;
    if (!(input >> returnedSource >> returnedDestination >> returnedValue) ||
        (input >> extra) || returnedSource != source ||
        returnedDestination != destination || returnedValue < 0 ||
        returnedValue > 1) {
        std::cerr << "fw1814ctl: invalid mixer-route response: " << payload
                  << '\n';
        return 1;
    }
    std::cout << kMixerSourceLabels[source] << " -> Mixer "
              << kMixerBusArgs[destination] << ": "
              << onOff(returnedValue != 0) << '\n';
    if (action == "set") {
        const std::string key = "mixer-route:" + std::string(argv[3]) +
                                ":" + argv[4];
        persistSuccessfulSet(argv[0], key, argc, argv);
    }
    return 0;
}

int softwareReturnLevelCommand(const std::string& action,
                               int argc,
                               char** argv) {
    const bool getting = action == "get";
    const bool settingAll = action == "set-all";
    const bool setting = action == "set";
    if ((getting && argc != 4) || (settingAll && argc != 5) ||
        (setting && argc != 5 && argc != 6) ||
        (!getting && !settingAll && !setting))
        return usage();

    const int pair = indexOf(argv[3], kMixerSourceArgs.data(),
                             kMixerSourceArgs.size());
    if (pair < 0) return usage();

    int level = -1;
    int leftRaw = 0;
    int rightRaw = 0;
    if (settingAll) {
        const std::string value = argv[4];
        level = value == "mute" ? 0 : value == "unity" ? 1 : -1;
        if (level < 0) return usage();
    } else if (setting) {
        if (!dbToRaw(argv[4], leftRaw)) return usage();
        if (argc == 6) {
            if (!dbToRaw(argv[5], rightRaw)) return usage();
        } else {
            rightRaw = leftRaw;
        }
    }

    std::string command = "SOFTWARE_RETURN_LEVEL " +
        std::string(getting ? "GET " : settingAll ? "SET_ALL " : "SET ") +
        std::to_string(pair);
    if (settingAll)
        command += " " + std::to_string(level);
    else if (setting)
        command += " " + std::to_string(leftRaw) + " " +
                   std::to_string(rightRaw);

    std::string payload;
    if (!payloadFor(command, payload)) return 1;
    std::istringstream input(payload);
    int returnedPair = -1;
    int returnedLeft = 0;
    int returnedRight = 0;
    std::string raw;
    std::string extra;
    if (!(input >> returnedPair >> returnedLeft >> returnedRight >> raw) ||
        (input >> extra) || returnedPair != pair || returnedLeft < -32768 ||
        returnedLeft > 0 || returnedRight < -32768 || returnedRight > 0 ||
        returnedLeft % 0x100 != 0 || returnedRight % 0x100 != 0) {
        std::cerr << "fw1814ctl: invalid software-return-level response: "
                  << payload << '\n';
        return 1;
    }

    std::uint32_t rawValue = 0;
    if (!parseRawWord(raw, rawValue) ||
        macfw::fw1814::monitorLevelRaw(
            macfw::fw1814::monitorLevelChannel(rawValue, 0)) !=
            returnedLeft ||
        macfw::fw1814::monitorLevelRaw(
            macfw::fw1814::monitorLevelChannel(rawValue, 1)) !=
            returnedRight) {
        std::cerr << "fw1814ctl: invalid software return gain value\n";
        return 1;
    }

    constexpr std::array<const char*, 2> kRegisterNames{{
        "GAIN_STM_34_IN", "GAIN_STM_12_IN",
    }};
    std::cout << kMixerSourceLabels[pair] << " level:\n"
              << "  left:  " << rawToDb(returnedLeft)
              << " (raw " << returnedLeft << ")\n"
              << "  right: " << rawToDb(returnedRight)
              << " (raw " << returnedRight << ")\n"
              << kRegisterNames[pair] << ": " << raw
              << " (write-only cache)\n";
    if (setting || settingAll)
        persistSuccessfulSet(
            argv[0], "software-return-level:" + std::string(argv[3]),
            argc, argv);
    return 0;
}

int inputMixerGet() {
    std::string payload;
    if (!payloadFor("INPUT_MIXER GET", payload)) return 1;

    std::istringstream input(payload);
    std::array<std::array<unsigned, RoutingModel::kMixerBusCount>,
               RoutingModel::kAnalogInputPairCount> routes{};
    for (auto& source : routes)
        for (auto& destination : source)
            if (!(input >> destination) || destination > 1) {
                std::cerr << "fw1814ctl: invalid input-mixer response: "
                          << payload << '\n';
                return 1;
            }

    std::string raw;
    std::string extra;
    if (!(input >> raw) || (input >> extra)) {
        std::cerr << "fw1814ctl: invalid input-mixer response: " << payload
                  << '\n';
        return 1;
    }
    std::uint32_t rawValue = 0;
    if (!parseRawWord(raw, rawValue) || rawValue > 0xffu) {
        std::cerr << "fw1814ctl: invalid MIX_ANA_DIG_IN value\n";
        return 1;
    }

    std::cout << "FW1814 analog input mixer (active engine cache):\n";
    for (std::size_t source = 0; source < routes.size(); ++source) {
        std::cout << "  " << kInputPairLabels[source] << ":";
        for (std::size_t destination = 0;
             destination < routes[source].size(); ++destination)
            std::cout << "  " << kMixerBusArgs[destination] << '='
                      << onOff(routes[source][destination] != 0);
        std::cout << '\n';
    }
    std::cout << "  MIX_ANA_DIG_IN: " << raw
              << " (write-only cache)\n";
    return 0;
}

int inputMixerRouteCommand(const std::string& action,
                           int argc,
                           char** argv) {
    if ((action == "get" && argc != 5) ||
        (action == "set" && argc != 6))
        return usage();
    const int source = indexOf(argv[3], kInputPairArgs.data(),
                               kInputPairArgs.size());
    const int destination = indexOf(argv[4], kMixerBusArgs.data(),
                                    kMixerBusArgs.size());
    if (source < 0 || destination < 0) return usage();

    int value = -1;
    if (action == "set") {
        const std::string state = argv[5];
        value = state == "on" ? 1 : state == "off" ? 0 : -1;
        if (value < 0) return usage();
    }

    std::string command = "INPUT_MIXER ROUTE " +
        std::string(action == "get" ? "GET " : "SET ") +
        std::to_string(source) + " " + std::to_string(destination);
    if (action == "set") command += " " + std::to_string(value);

    std::string payload;
    if (!payloadFor(command, payload)) return 1;
    std::istringstream input(payload);
    int returnedSource = -1;
    int returnedDestination = -1;
    int returnedValue = -1;
    std::string raw;
    std::string extra;
    if (!(input >> returnedSource >> returnedDestination >> returnedValue >>
          raw) || (input >> extra) || returnedSource != source ||
        returnedDestination != destination || returnedValue < 0 ||
        returnedValue > 1) {
        std::cerr << "fw1814ctl: invalid input-mixer-route response: "
                  << payload << '\n';
        return 1;
    }
    std::uint32_t rawValue = 0;
    if (!parseRawWord(raw, rawValue) || rawValue > 0xffu) {
        std::cerr << "fw1814ctl: invalid MIX_ANA_DIG_IN value\n";
        return 1;
    }

    std::cout << kInputPairLabels[source] << " -> Mixer "
              << kMixerBusArgs[destination] << ": "
              << onOff(returnedValue != 0) << '\n'
              << "MIX_ANA_DIG_IN: " << raw
              << " (write-only cache)\n";
    if (action == "set") {
        const std::string key = "input-mixer-route:" +
                                std::string(argv[3]) + ":" + argv[4];
        persistSuccessfulSet(argv[0], key, argc, argv);
    }
    return 0;
}

int inputMonitorLevelCommand(const std::string& action,
                             int argc,
                             char** argv) {
    const bool getting = action == "get";
    const bool settingAll = action == "set-all";
    const bool setting = action == "set";
    if ((getting && argc != 4) || (settingAll && argc != 5) ||
        (setting && argc != 5 && argc != 6) ||
        (!getting && !settingAll && !setting))
        return usage();

    const int pair = indexOf(argv[3], kInputPairArgs.data(),
                             kInputPairArgs.size());
    if (pair < 0) return usage();

    int level = -1;
    int leftRaw = 0;
    int rightRaw = 0;
    if (settingAll) {
        const std::string value = argv[4];
        level = value == "mute" ? 0
              : value == "unity" ? 1
              : value == "-20db" ? 2
              : -1;
        if (level < 0) return usage();
    } else if (setting) {
        if (!dbToRaw(argv[4], leftRaw)) return usage();
        if (argc == 6) {
            if (!dbToRaw(argv[5], rightRaw)) return usage();
        } else {
            rightRaw = leftRaw;
        }
    }

    std::string command = "INPUT_MONITOR_LEVEL " +
        std::string(getting ? "GET " : settingAll ? "SET_ALL " : "SET ") +
        std::to_string(pair);
    if (settingAll)
        command += " " + std::to_string(level);
    else if (setting)
        command += " " + std::to_string(leftRaw) + " " +
                   std::to_string(rightRaw);

    std::string payload;
    if (!payloadFor(command, payload)) return 1;
    std::istringstream input(payload);
    int returnedPair = -1;
    int returnedLeft = 0;
    int returnedRight = 0;
    std::string raw;
    std::string extra;
    if (!(input >> returnedPair >> returnedLeft >> returnedRight >> raw) ||
        (input >> extra) || returnedPair != pair || returnedLeft < -32768 ||
        returnedLeft > 0 || returnedRight < -32768 || returnedRight > 0 ||
        returnedLeft % 0x100 != 0 || returnedRight % 0x100 != 0) {
        std::cerr << "fw1814ctl: invalid input-monitor-level response: "
                  << payload << '\n';
        return 1;
    }

    std::uint32_t rawValue = 0;
    if (!parseRawWord(raw, rawValue) ||
        macfw::fw1814::monitorLevelRaw(
            macfw::fw1814::monitorLevelChannel(rawValue, 0)) !=
            returnedLeft ||
        macfw::fw1814::monitorLevelRaw(
            macfw::fw1814::monitorLevelChannel(rawValue, 1)) !=
            returnedRight) {
        std::cerr << "fw1814ctl: invalid analog input gain value\n";
        return 1;
    }

    constexpr std::array<const char*, 4> kRegisterNames{{
        "GAIN_ANA_12_IN", "GAIN_ANA_34_IN", "GAIN_ANA_56_IN",
        "GAIN_ANA_78_IN",
    }};
    std::cout << kInputPairLabels[pair] << " monitor level:\n"
              << "  left:  " << rawToDb(returnedLeft)
              << " (raw " << returnedLeft << ")\n"
              << "  right: " << rawToDb(returnedRight)
              << " (raw " << returnedRight << ")\n"
              << kRegisterNames[pair] << ": " << raw
              << " (write-only cache)\n";
    if (setting || settingAll)
        persistSuccessfulSet(
            argv[0], "input-monitor-level:" + std::string(argv[3]),
            argc, argv);
    return 0;
}

int inputMonitorChannelLevelCommand(const std::string& action,
                                    int argc,
                                    char** argv) {
    const bool getting = action == "get";
    const bool setting = action == "set";
    if ((getting && argc != 5) || (setting && argc != 6) ||
        (!getting && !setting))
        return usage();

    const int pair = indexOf(argv[3], kInputPairArgs.data(),
                             kInputPairArgs.size());
    if (pair < 0) return usage();

    constexpr std::array<const char*, 2> kChannelArgs{{"left", "right"}};
    const int channel = indexOf(argv[4], kChannelArgs.data(),
                                kChannelArgs.size());
    if (channel < 0) return usage();

    int level = -1;
    if (setting) {
        const std::string value = argv[5];
        level = value == "unity" ? 1 : value == "-20db" ? 2 : -1;
        if (level < 0) return usage();
    }

    std::string command = "INPUT_MONITOR_CHANNEL_LEVEL " +
        std::string(getting ? "GET " : "SET ") +
        std::to_string(pair) + " " +
        std::to_string(channel);
    if (setting) command += " " + std::to_string(level);

    std::string payload;
    if (!payloadFor(command, payload)) return 1;
    std::istringstream input(payload);
    int returnedPair = -1;
    int returnedChannel = -1;
    int returnedRaw = 0;
    std::string raw;
    std::string extra;
    if (!(input >> returnedPair >> returnedChannel >> returnedRaw >> raw) ||
        (input >> extra) || returnedPair != pair ||
        returnedChannel != channel || returnedRaw < -32768 ||
        returnedRaw > 0 || returnedRaw % 0x100 != 0) {
        std::cerr << "fw1814ctl: invalid input-monitor-channel-level "
                     "response: " << payload << '\n';
        return 1;
    }

    std::uint32_t rawValue = 0;
    if (!parseRawWord(raw, rawValue)) {
        std::cerr << "fw1814ctl: invalid analog input gain value\n";
        return 1;
    }
    if (macfw::fw1814::monitorLevelChannel(
            rawValue, static_cast<std::size_t>(channel)) !=
        static_cast<std::uint16_t>(returnedRaw)) {
        std::cerr << "fw1814ctl: inconsistent analog input channel gain\n";
        return 1;
    }

    constexpr std::array<const char*, 4> kRegisterNames{{
        "GAIN_ANA_12_IN", "GAIN_ANA_34_IN", "GAIN_ANA_56_IN",
        "GAIN_ANA_78_IN",
    }};
    std::cout << kInputPairLabels[pair] << " " << kChannelArgs[channel]
              << " channel monitor level: " << rawToDb(returnedRaw) << '\n'
              << kRegisterNames[pair] << ": " << raw
              << " (write-only diagnostic cache)\n";
    return 0;
}

int inputMonitorPanCommand(const std::string& action,
                           int argc,
                           char** argv) {
    const bool getting = action == "get";
    const bool setting = action == "set";
    const bool settingPercent = action == "set-percent";
    if ((getting && argc != 4) ||
        ((setting || settingPercent) && argc != 6) ||
        (!getting && !setting && !settingPercent))
        return usage();

    const int pair = indexOf(argv[3], kInputPairArgs.data(),
                             kInputPairArgs.size());
    if (pair < 0 || pair > 3) return usage();

    constexpr std::array<const char*, 2> kChannelArgs{{"left", "right"}};
    constexpr std::array<const char*, 3> kPositionArgs{{
        "left", "center", "right",
    }};
    int channel = -1;
    int position = -1;
    if (setting || settingPercent) {
        channel = indexOf(argv[4], kChannelArgs.data(), kChannelArgs.size());
        if (channel < 0) return usage();
        if (setting)
            position = indexOf(
                argv[5], kPositionArgs.data(), kPositionArgs.size());
        else if (!parseSignedRange(argv[5], -100, 100, position))
            return usage();
        if (setting && position < 0)
            return usage();
    }

    const char* wireAction = getting ? "GET "
                           : setting ? "SET " : "SET_PERCENT ";
    std::string command = "INPUT_MONITOR_PAN " + std::string(wireAction) +
                          std::to_string(pair);
    if (setting || settingPercent)
        command += " " + std::to_string(channel) + " " +
                   std::to_string(position);

    std::string payload;
    if (!payloadFor(command, payload)) return 1;
    std::istringstream input(payload);
    int returnedPair = -1;
    int leftPercent = 0;
    int rightPercent = 0;
    std::string raw;
    std::string extra;
    if (!(input >> returnedPair >> leftPercent >> rightPercent >> raw) ||
        (input >> extra) || returnedPair != pair || leftPercent < -100 ||
        leftPercent > 100 || rightPercent < -100 || rightPercent > 100) {
        std::cerr << "fw1814ctl: invalid input-monitor-pan response: "
                  << payload << '\n';
        return 1;
    }

    std::uint32_t rawValue = 0;
    if (!parseRawWord(raw, rawValue) ||
        macfw::fw1814::inputPanChannel(rawValue, 0) !=
            macfw::fw1814::inputPanFromPercent(leftPercent) ||
        macfw::fw1814::inputPanChannel(rawValue, 1) !=
            macfw::fw1814::inputPanFromPercent(rightPercent)) {
        std::cerr << "fw1814ctl: inconsistent analog input pan value\n";
        return 1;
    }

    constexpr std::array<const char*, 4> kRegisterNames{{
        "LR_ANA_12_IN", "LR_ANA_34_IN", "LR_ANA_56_IN", "LR_ANA_78_IN",
    }};
    const auto panLabel = [](int percent) {
        if (percent == -100) return std::string("left");
        if (percent == 0) return std::string("center");
        if (percent == 100) return std::string("right");
        if (percent < 0)
            return std::to_string(-percent) + "% left";
        return std::to_string(percent) + "% right";
    };
    std::cout << kInputPairLabels[pair] << " monitor pan: left-channel="
              << panLabel(leftPercent) << " right-channel="
              << panLabel(rightPercent) << '\n'
              << kRegisterNames[pair] << ": " << raw
              << " (write-only cache)\n";
    if (setting || settingPercent) {
        const std::string key = "input-monitor-pan:" +
                                std::string(argv[3]) + ":" +
                                std::string(argv[4]);
        persistSuccessfulSet(argv[0], key, argc, argv);
    }
    return 0;
}

int outputStateGet() {
    std::string payload;
    if (!payloadFor("OUTPUT GET", payload)) return 1;
    std::istringstream input(payload);
    std::array<unsigned, RoutingModel::kAnalogOutputPairCount> sources{};
    for (auto& source : sources)
        if (!(input >> source) || source > 1) {
            std::cerr << "fw1814ctl: invalid output response: " << payload
                      << '\n';
            return 1;
        }
    std::string raw;
    std::string extra;
    if (!(input >> raw) || (input >> extra)) {
        std::cerr << "fw1814ctl: invalid output response: " << payload << '\n';
        return 1;
    }
    std::cout << "FW1814 analog output sources (active engine cache):\n";
    for (std::size_t pair = 0; pair < sources.size(); ++pair)
        std::cout << "  " << kOutputPairLabels[pair] << ": "
                  << outputSourceName(sources[pair]) << '\n';
    std::cout << "  SRC_ANA_OUT: " << raw << " (write-only cache)\n";
    return 0;
}

int outputSourceCommand(const std::string& action, int argc, char** argv) {
    if ((action == "get" && argc != 4) ||
        (action == "set" && argc != 5))
        return usage();
    const int pair = indexOf(argv[3], kOutputPairArgs.data(),
                             kOutputPairArgs.size());
    if (pair < 0) return usage();

    int source = -1;
    if (action == "set") {
        const std::string value = argv[4];
        source = value == "mixer" ? 0 : value == "aux" ? 1 : -1;
        if (source < 0) return usage();
    }

    std::string command = "OUTPUT SOURCE " +
        std::string(action == "get" ? "GET " : "SET ") +
        std::to_string(pair);
    if (action == "set") command += " " + std::to_string(source);

    std::string payload;
    if (!payloadFor(command, payload)) return 1;
    std::istringstream input(payload);
    int returnedPair = -1;
    int returnedSource = -1;
    std::string extra;
    if (!(input >> returnedPair >> returnedSource) || (input >> extra) ||
        returnedPair != pair || returnedSource < 0 || returnedSource > 1) {
        std::cerr << "fw1814ctl: invalid output-source response: " << payload
                  << '\n';
        return 1;
    }
    std::cout << kOutputPairLabels[pair] << ": "
              << outputSourceName(static_cast<unsigned>(returnedSource))
              << '\n';
    if (action == "set")
        persistSuccessfulSet(argv[0],
                             "output-source:" + std::string(argv[3]),
                             argc, argv);
    return 0;
}

int printHeadphoneState(const std::string& payload) {
    std::istringstream input(payload);
    unsigned first = 0;
    unsigned second = 0;
    std::string raw;
    std::string extra;
    if (!(input >> first >> second >> raw) || (input >> extra) ||
        first > 2 || second > 2) {
        std::cerr << "fw1814ctl: invalid headphone response: " << payload
                  << '\n';
        return 1;
    }
    std::uint32_t rawValue = 0;
    if (!parseRawWord(raw, rawValue)) {
        std::cerr << "fw1814ctl: invalid SRC_HP_OUT value\n";
        return 1;
    }
    std::cout << "FW1814 headphone sources (active engine cache):\n"
              << "  Headphone Output 1: " << headphoneSourceName(first)
              << '\n'
              << "  Headphone Output 2: " << headphoneSourceName(second)
              << '\n'
              << "  SRC_HP_OUT: " << raw << " (write-only cache)\n";
    return 0;
}

int headphoneStateGet() {
    std::string payload;
    if (!payloadFor("HEADPHONE GET", payload)) return 1;
    return printHeadphoneState(payload);
}

int headphoneSourceCommand(const std::string& action,
                           int argc,
                           char** argv) {
    if ((action == "get" && argc != 4) ||
        (action == "set" && argc != 5))
        return usage();
    const int output = indexOf(argv[3], kHeadphoneOutputArgs.data(),
                               kHeadphoneOutputArgs.size());
    if (output < 0) return usage();

    int source = -1;
    if (action == "set") {
        source = indexOf(argv[4], kHeadphoneSourceArgs.data(), 2);
        if (source < 0) return usage();
    }

    std::string command = "HEADPHONE SOURCE " +
        std::string(action == "get" ? "GET " : "SET ") +
        std::to_string(output);
    if (action == "set") command += " " + std::to_string(source);

    std::string payload;
    if (!payloadFor(command, payload)) return 1;
    std::istringstream input(payload);
    int returnedOutput = -1;
    int returnedSource = -1;
    std::string raw;
    std::string extra;
    if (!(input >> returnedOutput >> returnedSource >> raw) ||
        (input >> extra) || returnedOutput != output ||
        returnedSource < 0 || returnedSource > 2) {
        std::cerr << "fw1814ctl: invalid headphone-source response: "
                  << payload << '\n';
        return 1;
    }
    std::uint32_t rawValue = 0;
    if (!parseRawWord(raw, rawValue)) {
        std::cerr << "fw1814ctl: invalid SRC_HP_OUT value\n";
        return 1;
    }
    std::cout << kHeadphoneOutputLabels[output] << ": "
              << headphoneSourceName(static_cast<unsigned>(returnedSource))
              << '\n'
              << "SRC_HP_OUT: " << raw << " (write-only cache)\n";
    if (action == "set")
        persistSuccessfulSet(argv[0],
                             "headphone-source:" + std::string(argv[3]),
                             argc, argv);
    return 0;
}

int capabilitiesGet() {
    std::string payload;
    if (!payloadFor("CAPABILITIES GET", payload)) return 1;
    std::cout << "FW1814 control capabilities:\n";
    std::istringstream input(payload);
    std::string item;
    while (input >> item) std::cout << "  " << item << '\n';
    return 0;
}

int engineGet() {
    std::string payload;
    if (!payloadFor("ENGINE GET", payload)) return 1;
    std::istringstream input(payload);
    unsigned rate = 0;
    unsigned generation = 0;
    std::string extra;
    if (!(input >> rate >> generation) || (input >> extra)) {
        std::cerr << "fw1814ctl: invalid engine response: " << payload
                  << '\n';
        return 1;
    }
    std::cout << "FW1814 engine:\n"
              << "  sample rate: " << rate << " Hz\n"
              << "  FireWire generation at control startup: " << generation
              << '\n';
    return 0;
}

int internalReady() {
    std::string payload;
    if (!payloadFor("CONTROL READY", payload)) return 1;
    if (payload != "ready") {
        std::cerr << "fw1814ctl: invalid control-ready response: "
                  << payload << '\n';
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "internal-ready")
        return internalReady();
    if (argc < 3) return usage();
    const std::string control = argv[1];
    const std::string action = argv[2];

    if (control == "routing")
        return action == "get" && argc == 3 ? routingGet() : usage();
    if (control == "mixer")
        return action == "get" && argc == 3 ? mixerGet() : usage();
    if (control == "mixer-route")
        return action == "get" || action == "set"
            ? mixerRouteCommand(action, argc, argv)
            : usage();
    if (control == "software-return-level")
        return softwareReturnLevelCommand(action, argc, argv);
    if (control == "input-mixer")
        return action == "get" && argc == 3 ? inputMixerGet() : usage();
    if (control == "input-mixer-route")
        return action == "get" || action == "set"
            ? inputMixerRouteCommand(action, argc, argv)
            : usage();
    if (control == "input-monitor-level")
        return inputMonitorLevelCommand(action, argc, argv);
    if (control == "input-monitor-channel-level")
        return inputMonitorChannelLevelCommand(action, argc, argv);
    if (control == "input-monitor-pan")
        return inputMonitorPanCommand(action, argc, argv);
    if (control == "output-state")
        return action == "get" && argc == 3 ? outputStateGet() : usage();
    if (control == "output-source")
        return action == "get" || action == "set"
            ? outputSourceCommand(action, argc, argv)
            : usage();
    if (control == "headphone-state")
        return action == "get" && argc == 3 ? headphoneStateGet() : usage();
    if (control == "headphone-source")
        return action == "get" || action == "set"
            ? headphoneSourceCommand(action, argc, argv)
            : usage();
    if (control == "capabilities")
        return action == "get" && argc == 3 ? capabilitiesGet() : usage();
    if (control == "engine")
        return action == "get" && argc == 3 ? engineGet() : usage();
    return usage();
}
