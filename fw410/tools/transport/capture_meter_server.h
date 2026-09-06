#pragma once

#include "capture_shared.h"

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace macfw::transport {

class CaptureMeterServer {
public:
    static constexpr const char* kSocketPath = "/tmp/macfw-fw410-meter.sock";

    ~CaptureMeterServer() { reset(); }

    bool start(CaptureReceivePump& pump) {
        reset();

        // Meter and control sockets live inside the real-time transport process.
        // A GUI/client can legitimately disappear during a rate switch; never let
        // a late socket reply terminate the whole audio engine with SIGPIPE.
        std::signal(SIGPIPE, SIG_IGN);

        pump_ = &pump;
        listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;
        int flags = fcntl(listenFd_, F_GETFL, 0);
        if (flags >= 0) fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        if (std::strlen(kSocketPath) >= sizeof(a.sun_path)) {
            reset();
            return false;
        }
        std::strncpy(a.sun_path, kSocketPath, sizeof(a.sun_path) - 1);
        unlink(kSocketPath);
        if (bind(listenFd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            reset();
            return false;
        }
        chmod(kSocketPath, 0666);
        if (listen(listenFd_, 4) != 0) {
            reset();
            return false;
        }
        std::printf("FW410 meter socket: %s\n", kSocketPath);
        return true;
    }

    void reset() {
        if (clientFd_ >= 0) close(clientFd_);
        clientFd_ = -1;
        request_.clear();
        if (listenFd_ >= 0) close(listenFd_);
        listenFd_ = -1;
        unlink(kSocketPath);
        pump_ = nullptr;
    }

    void service() {
        if (listenFd_ < 0 || !pump_) return;
        if (clientFd_ < 0) {
            clientFd_ = accept(listenFd_, nullptr, nullptr);
            if (clientFd_ >= 0) {
                int flags = fcntl(clientFd_, F_GETFL, 0);
                if (flags >= 0) fcntl(clientFd_, F_SETFL, flags | O_NONBLOCK);
                request_.clear();
            } else {
                return;
            }
        }

        char b[128];
        const ssize_t n = recv(clientFd_, b, sizeof(b), 0);
        if (n > 0) {
            request_.append(b, static_cast<std::size_t>(n));
            const auto nl = request_.find('\n');
            if (nl != std::string::npos) {
                handle(request_.substr(0, nl));
                finishClient();
            }
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            finishClient();
        }
    }

private:
    static double toDbfs(float peak) {
        constexpr double kFloorDb = -120.0;
        if (!(peak > 0.0f)) return kFloorDb;
        const double db = 20.0 * std::log10(static_cast<double>(peak));
        return db < kFloorDb ? kFloorDb : (db > 0.0 ? 0.0 : db);
    }

    void finishClient() {
        if (clientFd_ >= 0) close(clientFd_);
        clientFd_ = -1;
        request_.clear();
    }

    void reply(const std::string& text) {
        if (clientFd_ < 0) return;
        send(clientFd_, text.data(), text.size(), 0);
    }

    void handle(const std::string& command) {
        if (command != "METERS GET") {
            reply("ERR unknown-command\n");
            return;
        }
        const auto& p = pump_->meterPeaks();
        char out[256] = {};
        std::snprintf(out, sizeof(out), "OK %.2f %.2f %.2f %.2f\n",
                      toDbfs(p[0]), toDbfs(p[1]), toDbfs(p[2]), toDbfs(p[3]));
        reply(out);
    }

    CaptureReceivePump* pump_ = nullptr;
    int listenFd_ = -1;
    int clientFd_ = -1;
    std::string request_;
};

} // namespace macfw::transport
