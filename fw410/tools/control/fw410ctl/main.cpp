#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr const char* kSocketPath = "/var/run/macfw-fw410-control.sock";

int usage() {
    std::cerr << "usage:\n"
              << "  fw410ctl headphone-source get\n"
              << "  fw410ctl headphone-source set 0|1\n";
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
}

int main(int argc, char** argv) {
    if (argc < 3 || std::string(argv[1]) != "headphone-source") return usage();

    std::string command;
    const std::string action = argv[2];
    if (action == "get" && argc == 3) {
        command = "HEADPHONE_SOURCE GET";
    } else if (action == "set" && argc == 4) {
        const std::string value = argv[3];
        if (value != "0" && value != "1") return usage();
        command = "HEADPHONE_SOURCE SET " + value;
    } else {
        return usage();
    }

    std::string response;
    if (!transact(command, response)) return 1;
    if (!response.empty() && response.back() == '\n') response.pop_back();

    if (response.rfind("OK ", 0) == 0) {
        std::cout << response.substr(3) << '\n';
        return 0;
    }

    std::cerr << "fw410ctl: " << response << '\n';
    return 1;
}
