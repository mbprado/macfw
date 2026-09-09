#include "../../../special_mixer_model.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using RoutingModel = macfw::fw1814::SpecialMixerRoutingModel;

constexpr const char* kSocketPath = "/tmp/macfw-fw1814-control.sock";
constexpr std::array<const char*, RoutingModel::kStreamSourceCount>
    kMixerSourceArgs{{"sw1/2", "sw3/4"}};
constexpr std::array<const char*, RoutingModel::kStreamSourceCount>
    kMixerSourceLabels{{"SW Return 1/2", "SW Return 3/4"}};
constexpr std::array<const char*, RoutingModel::kMixerBusCount>
    kMixerBusArgs{{"1/2", "3/4"}};
constexpr std::array<const char*, RoutingModel::kAnalogOutputPairCount>
    kOutputPairLabels{{"Analog Outputs 1/2", "Analog Outputs 3/4"}};

int usage() {
    std::cerr
        << "usage:\n"
        << "  fw1814ctl routing get\n"
        << "  fw1814ctl mixer get\n"
        << "  fw1814ctl mixer-route get sw1/2|sw3/4 1/2|3/4\n"
        << "  fw1814ctl mixer-route set sw1/2|sw3/4 1/2|3/4 on|off\n"
        << "  fw1814ctl capabilities get\n"
        << "  fw1814ctl engine get\n\n"
        << "FW1814 mixer registers are write-only. The active transport "
           "establishes a known startup baseline and maintains the "
           "authoritative routing cache. Settings currently return to that "
           "baseline when the engine restarts.\n";
    return 64;
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

    const std::string request = command + "\n";
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

int routingGet() {
    std::string payload;
    if (!payloadFor("ROUTING GET", payload)) return 1;

    std::istringstream input(payload);
    std::string profile;
    unsigned rate = 0;
    std::string mixStreamText;
    std::string analogOutputText;
    std::string stateSource;
    std::string extra;
    if (!(input >> profile >> rate >> mixStreamText >> analogOutputText >>
          stateSource) || (input >> extra)) {
        std::cerr << "fw1814ctl: invalid routing response: " << payload
                  << '\n';
        return 1;
    }

    std::uint32_t mixStreamIn = 0;
    std::uint32_t srcAnalogOut = 0;
    if (!parseRawWord(mixStreamText, mixStreamIn) ||
        !parseRawWord(analogOutputText, srcAnalogOut)) {
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
    for (std::size_t pair = 0;
         pair < RoutingModel::kAnalogOutputPairCount; ++pair) {
        const unsigned source = (srcAnalogOut >> pair) & 1u;
        std::cout << "  " << kOutputPairLabels[pair] << ": "
                  << outputSourceName(source) << '\n';
    }
    std::cout << "  MIX_STM_IN: " << mixStreamText << '\n'
              << "  SRC_ANA_OUT: " << analogOutputText << '\n'
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

} // namespace

int main(int argc, char** argv) {
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
    if (control == "capabilities")
        return action == "get" && argc == 3 ? capabilitiesGet() : usage();
    if (control == "engine")
        return action == "get" && argc == 3 ? engineGet() : usage();
    return usage();
}
