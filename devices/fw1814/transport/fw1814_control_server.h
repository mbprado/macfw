#pragma once

#include "../special_mixer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace macfw::fw1814::transport {

// The transport owns the only open FireWire device handle.  Control clients
// therefore talk to this non-blocking socket instead of opening the interface
// themselves.  The initial API is deliberately read-only: FW1814 special
// mixer registers cannot be read back, so the engine reports the exact routing
// state that it established during startup.
class Fw1814ControlServer {
public:
    static constexpr const char* kSocketPath =
        "/tmp/macfw-fw1814-control.sock";

    ~Fw1814ControlServer() { reset(); }

    bool start(FireWireDevice& device, unsigned sampleRate) {
        reset();
        device_ = &device;
        sampleRate_ = sampleRate;
        generation_ = device.generation();

        listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;

        const int flags = fcntl(listenFd_, F_GETFL, 0);
        if (flags >= 0)
            fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (std::strlen(kSocketPath) >= sizeof(address.sun_path)) {
            reset();
            return false;
        }
        std::strncpy(address.sun_path, kSocketPath,
                     sizeof(address.sun_path) - 1);
        unlink(kSocketPath);
        if (bind(listenFd_, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) != 0) {
            reset();
            return false;
        }
        chmod(kSocketPath, 0666);
        if (listen(listenFd_, 4) != 0) {
            reset();
            return false;
        }

        std::printf("FW1814 control socket: %s\n", kSocketPath);
        return true;
    }

    void reset() {
        if (clientFd_ >= 0) close(clientFd_);
        clientFd_ = -1;
        request_.clear();
        if (listenFd_ >= 0) close(listenFd_);
        listenFd_ = -1;
        unlink(kSocketPath);
        device_ = nullptr;
        sampleRate_ = 0;
        generation_ = 0;
    }

    void service() {
        if (listenFd_ < 0 || !device_) return;

        if (clientFd_ < 0) {
            clientFd_ = accept(listenFd_, nullptr, nullptr);
            if (clientFd_ >= 0) {
                const int flags = fcntl(clientFd_, F_GETFL, 0);
                if (flags >= 0)
                    fcntl(clientFd_, F_SETFL, flags | O_NONBLOCK);
                request_.clear();
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                return;
            }
        }
        if (clientFd_ < 0) return;

        char buffer[256];
        const ssize_t count = recv(clientFd_, buffer, sizeof(buffer), 0);
        if (count > 0) {
            request_.append(buffer, static_cast<std::size_t>(count));
            if (request_.size() > 1024) {
                reply("ERR request-too-long\n");
                finishClient();
                return;
            }
            const auto newline = request_.find('\n');
            if (newline != std::string::npos) {
                handle(request_.substr(0, newline));
                finishClient();
            }
        } else if (count == 0 ||
                   (errno != EAGAIN && errno != EWOULDBLOCK)) {
            finishClient();
        }
    }

private:
    void finishClient() {
        if (clientFd_ >= 0) close(clientFd_);
        clientFd_ = -1;
        request_.clear();
    }

    void reply(const std::string& text) {
        if (clientFd_ < 0) return;
        const char* cursor = text.data();
        std::size_t remaining = text.size();
        while (remaining != 0) {
            const ssize_t count = send(clientFd_, cursor, remaining, 0);
            if (count <= 0) break;
            cursor += count;
            remaining -= static_cast<std::size_t>(count);
        }
    }

    void handle(const std::string& command) {
        if (command == "ROUTING GET") {
            reply("OK analog-straight " + std::to_string(sampleRate_) +
                  " 0x00000006 0x00000000 write-only\n");
            return;
        }
        if (command == "CAPABILITIES GET") {
            reply("OK routing-state=1 runtime-routing-set=0 "
                  "register-readback=0 digital=deferred "
                  "headphone=deferred midi=deferred\n");
            return;
        }
        if (command == "ENGINE GET") {
            reply("OK " + std::to_string(sampleRate_) + " " +
                  std::to_string(generation_) + "\n");
            return;
        }
        reply("ERR unknown-command\n");
    }

    FireWireDevice* device_ = nullptr;
    unsigned sampleRate_ = 0;
    UInt32 generation_ = 0;
    int listenFd_ = -1;
    int clientFd_ = -1;
    std::string request_;
};

} // namespace macfw::fw1814::transport
