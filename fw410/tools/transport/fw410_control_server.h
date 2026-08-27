#pragma once

#include "full_duplex_fcp_control.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace macfw::transport::duplex {

class Fw410ControlServer {
public:
    static constexpr const char* kSocketPath = "/tmp/macfw-fw410-control.sock";
    static constexpr std::uint8_t kHeadphoneSelector = 0x07;
    static constexpr std::uint8_t kHeadphoneLevel = 0x0f;
    static constexpr std::uint8_t kAuxStream12Level = 0x06;
    static constexpr std::uint8_t kAuxOutputLevel = 0x09;

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

    bool readStereoLevel(std::uint8_t functionBlock,
                         std::int16_t& left,
                         std::int16_t& right) {
        return fcp_->readLevel(functionBlock, 0, left) &&
               fcp_->readLevel(functionBlock, 1, right);
    }

    bool writeStereoLevel(std::uint8_t functionBlock,
                          std::int16_t left,
                          std::int16_t right) {
        if (!fcp_->writeLevel(functionBlock, 0, left)) return false;
        if (!fcp_->writeLevel(functionBlock, 1, right)) return false;
        std::int16_t verifyLeft = 0;
        std::int16_t verifyRight = 0;
        return readStereoLevel(functionBlock, verifyLeft, verifyRight) &&
               verifyLeft == left && verifyRight == right;
    }

    static bool parseRawLevel(const std::string& text, std::int16_t& value) {
        char* end = nullptr;
        errno = 0;
        const long parsed = std::strtol(text.c_str(), &end, 10);
        if (errno != 0 || !end || *end != '\0') return false;
        // Linux AvcLevelOperation exposes -inf..0 with 0x0100 (1 dB) steps.
        // Keep raw protocol access strict to that documented range/step.
        if (parsed == -32768) {
            value = static_cast<std::int16_t>(parsed);
            return true;
        }
        if (parsed < -32768 || parsed > 0 || (parsed % 0x100) != 0) return false;
        value = static_cast<std::int16_t>(parsed);
        return true;
    }

    void handleLevel(const std::string& command,
                     const std::string& prefix,
                     std::uint8_t functionBlock) {
        if (command == prefix + " GET") {
            std::int16_t left = 0;
            std::int16_t right = 0;
            if (!readStereoLevel(functionBlock, left, right)) {
                reply("ERR fcp-read-failed\n");
                return;
            }
            reply("OK " + std::to_string(left) + " " + std::to_string(right) + "\n");
            return;
        }

        const std::string setPrefix = prefix + " SET ";
        if (command.rfind(setPrefix, 0) != 0) return;

        std::istringstream input(command.substr(setPrefix.size()));
        std::string leftText;
        std::string rightText;
        std::string extra;
        if (!(input >> leftText >> rightText) || (input >> extra)) {
            reply("ERR invalid-level\n");
            return;
        }

        std::int16_t left = 0;
        std::int16_t right = 0;
        if (!parseRawLevel(leftText, left) || !parseRawLevel(rightText, right)) {
            reply("ERR invalid-level\n");
            return;
        }
        if (!writeStereoLevel(functionBlock, left, right)) {
            reply("ERR fcp-write-or-verify-failed\n");
            return;
        }
        reply("OK " + std::to_string(left) + " " + std::to_string(right) + "\n");
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

        if (command.rfind("HEADPHONE_VOLUME ", 0) == 0) {
            handleLevel(command, "HEADPHONE_VOLUME", kHeadphoneLevel);
            return;
        }
        if (command.rfind("AUX_STREAM12_VOLUME ", 0) == 0) {
            handleLevel(command, "AUX_STREAM12_VOLUME", kAuxStream12Level);
            return;
        }
        if (command.rfind("AUX_OUTPUT_VOLUME ", 0) == 0) {
            handleLevel(command, "AUX_OUTPUT_VOLUME", kAuxOutputLevel);
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
