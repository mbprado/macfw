#pragma once

#include "full_duplex_fcp_control.h"

#include <array>
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
    static constexpr std::uint8_t kHeadphoneMixerBlock = 0x07;
    static constexpr std::uint8_t kHeadphoneMixerInputPlug = 0x00;
    static constexpr std::uint8_t kHeadphoneMixerOutputChannel = 0x01;
    static constexpr std::array<std::uint8_t, 5> kHeadphoneMixerInputChannels = {
        0x01, 0x03, 0x05, 0x07, 0x09
    };

    // Linux snd-firewire-ctl-services Fw410PhysOutputProtocol mapping.
    // Pair order: Analog 1/2, 3/4, 5/6, 7/8, S/PDIF L/R.
    static constexpr std::array<std::uint8_t, 5> kOutputSelectorBlocks = {
        0x02, 0x03, 0x04, 0x05, 0x06
    };
    static constexpr std::array<std::uint8_t, 5> kOutputLevelBlocks = {
        0x0a, 0x0b, 0x0c, 0x0d, 0x0e
    };
    static constexpr std::uint8_t kSpdifConnectorSelector = 0x01;

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
        return fcp_->readLevel(functionBlock, 1, left) &&
               fcp_->readLevel(functionBlock, 2, right);
    }

    bool writeStereoLevel(std::uint8_t functionBlock,
                          std::int16_t left,
                          std::int16_t right) {
        if (!fcp_->writeLevel(functionBlock, 1, left)) return false;
        if (!fcp_->writeLevel(functionBlock, 2, right)) return false;
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

    bool readHeadphoneMixer(std::array<bool, 5>& state) {
        for (std::size_t i = 0; i < state.size(); ++i) {
            if (!fcp_->readProcessingMixer(kHeadphoneMixerBlock,
                                           kHeadphoneMixerInputPlug,
                                           kHeadphoneMixerInputChannels[i],
                                           kHeadphoneMixerOutputChannel,
                                           state[i]))
                return false;
        }
        return true;
    }

    bool writeHeadphoneMixer(std::size_t index, bool enabled) {
        if (index >= kHeadphoneMixerInputChannels.size()) return false;
        if (!fcp_->writeProcessingMixer(kHeadphoneMixerBlock,
                                        kHeadphoneMixerInputPlug,
                                        kHeadphoneMixerInputChannels[index],
                                        kHeadphoneMixerOutputChannel,
                                        enabled))
            return false;
        bool verify = false;
        return fcp_->readProcessingMixer(kHeadphoneMixerBlock,
                                         kHeadphoneMixerInputPlug,
                                         kHeadphoneMixerInputChannels[index],
                                         kHeadphoneMixerOutputChannel,
                                         verify) && verify == enabled;
    }

    void handleHeadphoneMixer(const std::string& command) {
        if (command == "HEADPHONE_MIXER GET") {
            std::array<bool, 5> state{};
            if (!readHeadphoneMixer(state)) {
                reply("ERR fcp-read-failed\n");
                return;
            }
            std::string out = "OK";
            for (bool enabled : state) out += enabled ? " 1" : " 0";
            reply(out + "\n");
            return;
        }

        const std::string prefix = "HEADPHONE_MIXER SET ";
        if (command.rfind(prefix, 0) != 0) {
            reply("ERR unknown-command\n");
            return;
        }

        std::istringstream input(command.substr(prefix.size()));
        unsigned index = 0;
        unsigned value = 0;
        std::string extra;
        if (!(input >> index >> value) || (input >> extra) || index >= 5 || value > 1) {
            reply("ERR invalid-headphone-mixer\n");
            return;
        }
        if (!writeHeadphoneMixer(index, value != 0)) {
            reply("ERR fcp-write-or-verify-failed\n");
            return;
        }
        reply("OK " + std::to_string(index) + " " + std::to_string(value) + "\n");
    }

    void handleOutputPair(const std::string& command) {
        const std::string getPrefix = "OUTPUT_PAIR GET ";
        if (command.rfind(getPrefix, 0) == 0) {
            std::istringstream input(command.substr(getPrefix.size()));
            unsigned index = 0;
            std::string extra;
            if (!(input >> index) || (input >> extra) || index >= kOutputSelectorBlocks.size()) {
                reply("ERR invalid-output-pair\n");
                return;
            }

            std::uint8_t source = 0xff;
            std::int16_t left = 0;
            std::int16_t right = 0;
            if (!fcp_->readSelector(kOutputSelectorBlocks[index], source) ||
                !readStereoLevel(kOutputLevelBlocks[index], left, right)) {
                reply("ERR fcp-read-failed\n");
                return;
            }

            reply("OK " + std::to_string(static_cast<unsigned>(source)) + " " +
                  std::to_string(left) + " " + std::to_string(right) + "\n");
            return;
        }

        const std::string setSourcePrefix = "OUTPUT_PAIR SET_SOURCE ";
        if (command.rfind(setSourcePrefix, 0) == 0) {
            std::istringstream input(command.substr(setSourcePrefix.size()));
            unsigned index = 0;
            unsigned source = 0;
            std::string extra;
            if (!(input >> index >> source) || (input >> extra) ||
                index >= kOutputSelectorBlocks.size() || source > 1) {
                reply("ERR invalid-output-source\n");
                return;
            }

            const auto block = kOutputSelectorBlocks[index];
            const auto value = static_cast<std::uint8_t>(source);
            if (!fcp_->writeSelector(block, value)) {
                reply("ERR fcp-write-failed\n");
                return;
            }
            std::uint8_t verify = 0xff;
            if (!fcp_->readSelector(block, verify) || verify != value) {
                reply("ERR verify-failed\n");
                return;
            }
            reply("OK " + std::to_string(index) + " " +
                  std::to_string(static_cast<unsigned>(verify)) + "\n");
            return;
        }

        reply("ERR unknown-command\n");
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

        if (command == "SPDIF_CONNECTOR GET") {
            std::uint8_t value = 0xff;
            if (!fcp_->readSelector(kSpdifConnectorSelector, value)) {
                reply("ERR fcp-read-failed\n");
                return;
            }
            reply("OK " + std::to_string(static_cast<unsigned>(value)) + "\n");
            return;
        }

        if (command.rfind("OUTPUT_PAIR ", 0) == 0) {
            handleOutputPair(command);
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
        if (command.rfind("HEADPHONE_MIXER ", 0) == 0) {
            handleHeadphoneMixer(command);
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
