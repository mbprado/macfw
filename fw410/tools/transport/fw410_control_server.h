#pragma once

#include "full_duplex_fcp_control.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace macfw::transport::duplex {

class Fw410ControlServer {
public:
    static constexpr const char* kSocketPath = "/var/run/macfw-fw410-control.sock";
    static constexpr std::uint8_t kHeadphoneSelector = 7;

    ~Fw410ControlServer() { reset(); }

    bool start(Fw410FcpControl& fcp) {
        reset();
        fcp_ = &fcp;

        listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;

        const int flags = fcntl(listenFd_, F_GETFL, 0);
        if (flags >= 0) fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (std::strlen(kSocketPath) >= sizeof(address.sun_path)) {
            reset();
            return false;
        }
        std::strncpy(address.sun_path, kSocketPath, sizeof(address.sun_path) - 1);

        unlink(kSocketPath);
        if (bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            reset();
            return false;
        }
        chmod(kSocketPath, 0666);
        if (listen(listenFd_, 4) != 0) {
            reset();
            return false;
        }

        std::printf("FW410 control socket: %s\n", kSocketPath);
        return true;
    }

    void reset() {
        if (clientFd_ >= 0) close(clientFd_);
        clientFd_ = -1;
        request_.clear();
        if (listenFd_ >= 0) close(listenFd_);
        listenFd_ = -1;
        unlink(kSocketPath);
        fcp_ = nullptr;
    }

    void service() {
        if (listenFd_ < 0 || !fcp_) return;

        if (clientFd_ < 0) {
            clientFd_ = accept(listenFd_, nullptr, nullptr);
            if (clientFd_ >= 0) {
                const int flags = fcntl(clientFd_, F_GETFL, 0);
                if (flags >= 0) fcntl(clientFd_, F_SETFL, flags | O_NONBLOCK);
                request_.clear();
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                return;
            }
        }
        if (clientFd_ < 0) return;

        char buffer[256];
        const ssize_t n = recv(clientFd_, buffer, sizeof(buffer), 0);
        if (n > 0) {
            request_.append(buffer, static_cast<std::size_t>(n));
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
        } else if (n == 0) {
            finishClient();
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
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
        const char* p = text.data();
        std::size_t left = text.size();
        while (left > 0) {
            const ssize_t n = send(clientFd_, p, left, 0);
            if (n <= 0) break;
            p += n;
            left -= static_cast<std::size_t>(n);
        }
    }

    void handle(const std::string& command) {
        if (command == "HEADPHONE_SOURCE GET") {
            std::uint8_t value = 0xff;
            if (!fcp_->readSelector(kHeadphoneSelector, value)) {
                reply("ERR fcp-read-failed\n");
                return;
            }
            reply("OK " + std::to_string(static_cast<unsigned>(value)) + "\n");
            return;
        }

        if (command == "HEADPHONE_SOURCE SET 0" || command == "HEADPHONE_SOURCE SET 1") {
            const std::uint8_t value = command.back() == '1' ? 1 : 0;
            if (!fcp_->writeSelector(kHeadphoneSelector, value)) {
                reply("ERR fcp-write-failed\n");
                return;
            }
            std::uint8_t verify = 0xff;
            if (!fcp_->readSelector(kHeadphoneSelector, verify) || verify != value) {
                reply("ERR verify-failed\n");
                return;
            }
            reply("OK " + std::to_string(static_cast<unsigned>(verify)) + "\n");
            return;
        }

        reply("ERR unknown-command\n");
    }

    Fw410FcpControl* fcp_ = nullptr;
    int listenFd_ = -1;
    int clientFd_ = -1;
    std::string request_;
};

} // namespace macfw::transport::duplex
