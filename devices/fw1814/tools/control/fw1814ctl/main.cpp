#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr const char* kSocketPath = "/tmp/macfw-fw1814-control.sock";

int usage() {
    std::cerr
        << "usage:\n"
        << "  fw1814ctl routing get\n"
        << "  fw1814ctl capabilities get\n"
        << "  fw1814ctl engine get\n\n"
        << "The initial routing API is read-only. The FW1814 special mixer "
           "registers are write-only, so state is reported from the active "
           "transport engine's authoritative cache.\n";
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

int routingGet() {
    std::string payload;
    if (!payloadFor("ROUTING GET", payload)) return 1;

    std::istringstream input(payload);
    std::string profile;
    unsigned rate = 0;
    std::string mixStreamIn;
    std::string srcAnalogOut;
    std::string stateSource;
    std::string extra;
    if (!(input >> profile >> rate >> mixStreamIn >> srcAnalogOut >>
          stateSource) || (input >> extra)) {
        std::cerr << "fw1814ctl: invalid routing response: " << payload
                  << '\n';
        return 1;
    }

    std::cout
        << "FW1814 routing state (active engine cache):\n"
        << "  profile: " << profile << '\n'
        << "  sample rate: " << rate << " Hz\n"
        << "  Stream 1/2 -> Mixer 1/2\n"
        << "  Stream 3/4 -> Mixer 3/4\n"
        << "  Mixer 1/2 -> Analog 1/2\n"
        << "  Mixer 3/4 -> Analog 3/4\n"
        << "  MIX_STM_IN: " << mixStreamIn << '\n'
        << "  SRC_ANA_OUT: " << srcAnalogOut << '\n'
        << "  hardware readback: unavailable (" << stateSource << ")\n";
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
    if (argc != 3 || std::string(argv[2]) != "get") return usage();
    const std::string control = argv[1];
    if (control == "routing") return routingGet();
    if (control == "capabilities") return capabilitiesGet();
    if (control == "engine") return engineGet();
    return usage();
}
