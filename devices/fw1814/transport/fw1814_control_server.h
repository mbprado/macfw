#pragma once

#include "../special_mixer.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace macfw::fw1814::transport {

// The transport owns the only open FireWire device handle.  Control clients
// therefore talk to this non-blocking socket instead of opening the interface
// themselves. FW1814 special mixer registers cannot be read back, so the
// engine establishes a known baseline and treats its software model as the
// authoritative state for every later differential update.
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
        routing_.loadStraightAnalogPlaybackPreset();

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
        routing_.loadStraightAnalogPlaybackPreset();
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

    static std::string hex32(std::uint32_t value) {
        std::ostringstream output;
        output << "0x" << std::hex << std::setw(8) << std::setfill('0')
               << value;
        return output.str();
    }

    enum class WriteResult {
        Ok,
        GenerationChanged,
        WriteFailed,
    };

    WriteResult writeRegister(UInt32 addressLo, std::uint32_t value) {
        if (!device_ ||
            !macfw::fw1814::mixerGenerationMatches(*device_, generation_))
            return WriteResult::GenerationChanged;
        if (!macfw::fw1814::writeMixerRegister(*device_, addressLo, value))
            return WriteResult::WriteFailed;
        if (!macfw::fw1814::mixerGenerationMatches(*device_, generation_))
            return WriteResult::GenerationChanged;
        return WriteResult::Ok;
    }

    void replyWriteError(WriteResult result) {
        if (result == WriteResult::GenerationChanged)
            reply("ERR bus-generation-changed\n");
        else
            reply("ERR mixer-write-failed\n");
    }

    void handleMixer(const std::string& command) {
        using Model = macfw::fw1814::SpecialMixerRoutingModel;
        if (command == "MIXER GET") {
            std::string output = "OK";
            for (std::size_t source = 0;
                 source < Model::kStreamSourceCount; ++source) {
                for (std::size_t destination = 0;
                     destination < Model::kMixerBusCount; ++destination) {
                    const bool enabled = routing_.streamRoute(
                        static_cast<Model::StreamSource>(source),
                        static_cast<Model::MixerBus>(destination));
                    output += enabled ? " 1" : " 0";
                }
            }
            reply(output + " " + hex32(routing_.mixStreamIn()) + "\n");
            return;
        }

        const std::string getPrefix = "MIXER ROUTE GET ";
        const std::string setPrefix = "MIXER ROUTE SET ";
        const bool setting = command.rfind(setPrefix, 0) == 0;
        const bool getting = command.rfind(getPrefix, 0) == 0;
        if (!setting && !getting) {
            reply("ERR unknown-command\n");
            return;
        }

        const std::string arguments = command.substr(
            setting ? setPrefix.size() : getPrefix.size());
        std::istringstream input(arguments);
        unsigned source = 0;
        unsigned destination = 0;
        unsigned value = 0;
        std::string extra;
        if (!(input >> source >> destination) ||
            (setting && !(input >> value)) || (input >> extra) ||
            source >= Model::kStreamSourceCount ||
            destination >= Model::kMixerBusCount || value > 1) {
            reply("ERR invalid-mixer-route\n");
            return;
        }

        const auto sourceId = static_cast<Model::StreamSource>(source);
        const auto destinationId = static_cast<Model::MixerBus>(destination);
        if (getting) {
            reply("OK " + std::to_string(source) + " " +
                  std::to_string(destination) + " " +
                  (routing_.streamRoute(sourceId, destinationId) ? "1" : "0") +
                  "\n");
            return;
        }

        Model desired = routing_;
        desired.setStreamRoute(sourceId, destinationId, value != 0);
        if (desired.mixStreamIn() != routing_.mixStreamIn()) {
            const WriteResult result = writeRegister(
                macfw::fw1814::kMixStreamInLo, desired.mixStreamIn());
            if (result != WriteResult::Ok) {
                replyWriteError(result);
                return;
            }
            routing_ = desired;
        }
        reply("OK " + std::to_string(source) + " " +
              std::to_string(destination) + " " + std::to_string(value) +
              "\n");
    }

    void handle(const std::string& command) {
        if (command == "ROUTING GET") {
            const std::string profile = routing_.isStraightAnalogPlaybackPreset()
                ? "analog-straight"
                : "custom";
            reply("OK " + profile + " " + std::to_string(sampleRate_) +
                  " " + hex32(routing_.mixStreamIn()) + " " +
                  hex32(routing_.srcAnalogOut()) + " write-only\n");
            return;
        }
        if (command == "CAPABILITIES GET") {
            reply("OK routing-state=1 runtime-routing-set=1 "
                  "stream-mixer=1 analog-output-source=deferred "
                  "register-readback=0 state-cache=authoritative "
                  "analog-input-mixer=deferred digital=deferred "
                  "headphone=deferred levels=deferred midi=deferred\n");
            return;
        }
        if (command == "ENGINE GET") {
            reply("OK " + std::to_string(sampleRate_) + " " +
                  std::to_string(generation_) + "\n");
            return;
        }
        if (command.rfind("MIXER ", 0) == 0) {
            handleMixer(command);
            return;
        }
        reply("ERR unknown-command\n");
    }

    FireWireDevice* device_ = nullptr;
    unsigned sampleRate_ = 0;
    UInt32 generation_ = 0;
    macfw::fw1814::SpecialMixerRoutingModel routing_{};
    int listenFd_ = -1;
    int clientFd_ = -1;
    std::string request_;
};

} // namespace macfw::fw1814::transport
