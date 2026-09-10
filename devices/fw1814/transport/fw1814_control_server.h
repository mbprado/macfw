#pragma once

#include "../special_mixer.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
        restoringControlState_ = std::getenv("MACFW_ENGINE_READY_FD") != nullptr;
        routing_.loadStraightAnalogPlaybackPreset();
        analogInputMonitorLevelKnown_.fill(false);
        gainAnalogIn_.fill(0);
        analogInputMonitorLevelKnown_[0] = true;
        analogInputMonitorLevelKnown_[1] = true;
        analogInputMonitorLevelKnown_[2] = true;
        gainAnalogIn_[0] = macfw::fw1814::stereoMonitorLevelWord(
            macfw::fw1814::kMonitorLevelUnity);
        gainAnalogIn_[1] = macfw::fw1814::stereoMonitorLevelWord(
            macfw::fw1814::kMonitorLevelUnity);
        gainAnalogIn_[2] = macfw::fw1814::stereoMonitorLevelWord(
            macfw::fw1814::kMonitorLevelUnity);

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
        restoringControlState_ = false;
        routing_.loadStraightAnalogPlaybackPreset();
        analogInputMonitorLevelKnown_.fill(false);
        gainAnalogIn_.fill(0);
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

    void handleInputMixer(const std::string& command) {
        using Model = macfw::fw1814::SpecialMixerRoutingModel;

        if (command == "INPUT_MIXER GET") {
            std::string output = "OK";
            for (std::size_t source = 0;
                 source < Model::kAnalogInputPairCount; ++source) {
                for (std::size_t destination = 0;
                     destination < Model::kMixerBusCount; ++destination) {
                    const bool enabled = routing_.analogInputRoute(
                        static_cast<Model::AnalogInputPair>(source),
                        static_cast<Model::MixerBus>(destination));
                    output += enabled ? " 1" : " 0";
                }
            }
            reply(output + " " + hex32(routing_.mixAnalogDigitalIn()) +
                  "\n");
            return;
        }

        const std::string getPrefix = "INPUT_MIXER ROUTE GET ";
        const std::string setPrefix = "INPUT_MIXER ROUTE SET ";
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
            source >= Model::kAnalogInputPairCount ||
            destination >= Model::kMixerBusCount || value > 1) {
            reply("ERR invalid-input-mixer-route\n");
            return;
        }

        const auto sourceId = static_cast<Model::AnalogInputPair>(source);
        const auto destinationId = static_cast<Model::MixerBus>(destination);
        if (getting) {
            reply("OK " + std::to_string(source) + " " +
                  std::to_string(destination) + " " +
                  (routing_.analogInputRoute(sourceId, destinationId)
                       ? "1" : "0") + " " +
                  hex32(routing_.mixAnalogDigitalIn()) + "\n");
            return;
        }

        Model desired = routing_;
        desired.setAnalogInputRoute(sourceId, destinationId, value != 0);
        if (desired.mixAnalogDigitalIn() != routing_.mixAnalogDigitalIn()) {
            const WriteResult result = writeRegister(
                macfw::fw1814::kMixAnalogDigitalInLo,
                desired.mixAnalogDigitalIn());
            if (result != WriteResult::Ok) {
                replyWriteError(result);
                return;
            }
            routing_ = desired;
        }
        reply("OK " + std::to_string(source) + " " +
              std::to_string(destination) + " " + std::to_string(value) +
              " " + hex32(routing_.mixAnalogDigitalIn()) + "\n");
    }

    void handleOutput(const std::string& command) {
        using Model = macfw::fw1814::SpecialMixerRoutingModel;
        if (command == "OUTPUT GET") {
            std::string output = "OK";
            for (std::size_t pair = 0;
                 pair < Model::kAnalogOutputPairCount; ++pair) {
                const auto source = routing_.analogOutputSource(
                    static_cast<Model::AnalogOutputPair>(pair));
                output += source == Model::OutputSource::Aux ? " 1" : " 0";
            }
            reply(output + " " + hex32(routing_.srcAnalogOut()) + "\n");
            return;
        }

        const std::string getPrefix = "OUTPUT SOURCE GET ";
        const std::string setPrefix = "OUTPUT SOURCE SET ";
        const bool setting = command.rfind(setPrefix, 0) == 0;
        const bool getting = command.rfind(getPrefix, 0) == 0;
        if (!setting && !getting) {
            reply("ERR unknown-command\n");
            return;
        }

        const std::string arguments = command.substr(
            setting ? setPrefix.size() : getPrefix.size());
        std::istringstream input(arguments);
        unsigned pair = 0;
        unsigned source = 0;
        std::string extra;
        if (!(input >> pair) || (setting && !(input >> source)) ||
            (input >> extra) || pair >= Model::kAnalogOutputPairCount ||
            source > 1) {
            reply("ERR invalid-output-source\n");
            return;
        }

        const auto pairId = static_cast<Model::AnalogOutputPair>(pair);
        if (getting) {
            const auto cached = routing_.analogOutputSource(pairId);
            reply("OK " + std::to_string(pair) + " " +
                  (cached == Model::OutputSource::Aux ? "1" : "0") +
                  "\n");
            return;
        }

        Model desired = routing_;
        desired.setAnalogOutputSource(
            pairId, source == 0 ? Model::OutputSource::Mixer
                                : Model::OutputSource::Aux);
        if (desired.srcAnalogOut() != routing_.srcAnalogOut()) {
            const WriteResult result = writeRegister(
                macfw::fw1814::kSrcAnalogOutLo, desired.srcAnalogOut());
            if (result != WriteResult::Ok) {
                replyWriteError(result);
                return;
            }
            routing_ = desired;
        }
        reply("OK " + std::to_string(pair) + " " +
              std::to_string(source) + "\n");
    }

    void handleHeadphone(const std::string& command) {
        using Model = macfw::fw1814::SpecialMixerRoutingModel;
        using Source = macfw::fw1814::HeadphoneSource;
        const auto sourceIndex = [](Source source) {
            return source == Source::Mixer12 ? 0u
                 : source == Source::Mixer34 ? 1u
                                             : 2u;
        };
        const auto sourceFor = [](unsigned value) {
            return value == 0 ? Source::Mixer12
                 : value == 1 ? Source::Mixer34
                              : Source::Aux12;
        };

        if (command == "HEADPHONE GET") {
            const unsigned first = sourceIndex(routing_.headphoneSource(
                Model::HeadphoneOutput::Output1));
            const unsigned second = sourceIndex(routing_.headphoneSource(
                Model::HeadphoneOutput::Output2));
            reply("OK " + std::to_string(first) + " " +
                  std::to_string(second) + " " +
                  hex32(routing_.srcHeadphoneOut()) + "\n");
            return;
        }

        const std::string getPrefix = "HEADPHONE SOURCE GET ";
        const std::string setPrefix = "HEADPHONE SOURCE SET ";
        const bool getting = command.rfind(getPrefix, 0) == 0;
        const bool setting = command.rfind(setPrefix, 0) == 0;

        if (getting || setting) {
            std::istringstream input(command.substr(
                getting ? getPrefix.size() : setPrefix.size()));
            unsigned output = 0;
            unsigned source = 0;
            std::string extra;
            if (!(input >> output) || (setting && !(input >> source)) ||
                (input >> extra) || output >= Model::kHeadphoneOutputCount ||
                source > 1) {
                reply("ERR invalid-headphone-source\n");
                return;
            }

            const auto outputId = static_cast<Model::HeadphoneOutput>(output);
            if (getting) {
                reply("OK " + std::to_string(output) + " " +
                      std::to_string(sourceIndex(
                          routing_.headphoneSource(outputId))) + " " +
                      hex32(routing_.srcHeadphoneOut()) + "\n");
                return;
            }

            Model desired = routing_;
            desired.setHeadphoneSource(outputId, sourceFor(source));
            if (desired.srcHeadphoneOut() != routing_.srcHeadphoneOut()) {
                const WriteResult result = writeRegister(
                    macfw::fw1814::kSrcHeadphoneOutLo,
                    desired.srcHeadphoneOut());
                if (result != WriteResult::Ok) {
                    replyWriteError(result);
                    return;
                }
                routing_ = desired;
            }
            reply("OK " + std::to_string(output) + " " +
                  std::to_string(source) + " " +
                  hex32(routing_.srcHeadphoneOut()) + "\n");
            return;
        }

        reply("ERR unknown-command\n");
    }

    void handleInputMonitorLevel(const std::string& command) {
        const std::string getPrefix = "INPUT_MONITOR_LEVEL GET ";
        const std::string setPrefix = "INPUT_MONITOR_LEVEL SET_ALL ";
        const bool getting = command.rfind(getPrefix, 0) == 0;
        const bool setting = command.rfind(setPrefix, 0) == 0;
        if (!getting && !setting) {
            reply("ERR unknown-command\n");
            return;
        }

        std::istringstream input(command.substr(
            getting ? getPrefix.size() : setPrefix.size()));
        unsigned pair = 0;
        unsigned level = 0;
        std::string extra;
        if (!(input >> pair) || (setting && !(input >> level)) ||
            (input >> extra) || pair > 3 || level > 1) {
            reply("ERR invalid-input-monitor-level\n");
            return;
        }

        if (getting) {
            if (!analogInputMonitorLevelKnown_[pair]) {
                reply("ERR input-monitor-level-state-uninitialized\n");
                return;
            }
            const unsigned cachedLevel =
                gainAnalogIn_[pair] == macfw::fw1814::stereoMonitorLevelWord(
                    macfw::fw1814::kMonitorLevelMute) ? 0u : 1u;
            reply("OK " + std::to_string(pair) + " " +
                  std::to_string(cachedLevel) + " " +
                  hex32(gainAnalogIn_[pair]) + "\n");
            return;
        }

        const std::uint16_t channelLevel = level == 0
            ? macfw::fw1814::kMonitorLevelMute
            : macfw::fw1814::kMonitorLevelUnity;
        const std::uint32_t desired =
            macfw::fw1814::stereoMonitorLevelWord(channelLevel);
        const std::array<UInt32, 4> addresses{{
            macfw::fw1814::kGainAnalog12InLo,
            macfw::fw1814::kGainAnalog34InLo,
            macfw::fw1814::kGainAnalog56InLo,
            macfw::fw1814::kGainAnalog78InLo,
        }};
        const WriteResult result = writeRegister(addresses[pair], desired);
        if (result != WriteResult::Ok) {
            replyWriteError(result);
            return;
        }
        gainAnalogIn_[pair] = desired;
        analogInputMonitorLevelKnown_[pair] = true;
        reply("OK " + std::to_string(pair) + " " +
              std::to_string(level) + " " +
              hex32(gainAnalogIn_[pair]) + "\n");
    }

    void handle(const std::string& wireCommand) {
        constexpr const char* kRestorePrefix = "RESTORE ";
        const bool restoreCommand = wireCommand.rfind(kRestorePrefix, 0) == 0;
        const std::string command = restoreCommand
            ? wireCommand.substr(std::strlen(kRestorePrefix))
            : wireCommand;

        if (command == "CONTROL READY") {
            restoringControlState_ = false;
            reply("OK ready\n");
            return;
        }

        if (restoringControlState_ && !restoreCommand) {
            reply("ERR control-state-restoring\n");
            return;
        }

        if (command == "ROUTING GET") {
            const std::string profile = routing_.isStraightAnalogPlaybackPreset()
                ? "analog-straight"
                : "custom";
            reply("OK " + profile + " " + std::to_string(sampleRate_) +
                  " " + hex32(routing_.mixStreamIn()) + " " +
                  hex32(routing_.srcAnalogOut()) + " " +
                  hex32(routing_.srcHeadphoneOut()) + " " +
                  hex32(routing_.mixAnalogDigitalIn()) +
                  " write-only\n");
            return;
        }
        if (command == "CAPABILITIES GET") {
            reply("OK routing-state=1 runtime-routing-set=1 "
                  "stream-mixer=1 analog-output-source=1 "
                  "headphone-source=1 "
                  "register-readback=0 state-cache=authoritative "
                  "analog-input-mixer=1 digital=deferred "
                  "analog-input-monitor-level=1/2+3/4+5/6-persistent,7/8-diagnostic "
                  "headphone-levels=deferred levels=deferred midi=deferred\n");
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
        if (command.rfind("INPUT_MIXER ", 0) == 0) {
            handleInputMixer(command);
            return;
        }
        if (command.rfind("INPUT_MONITOR_LEVEL ", 0) == 0) {
            handleInputMonitorLevel(command);
            return;
        }
        if (command.rfind("OUTPUT ", 0) == 0) {
            handleOutput(command);
            return;
        }
        if (command.rfind("HEADPHONE ", 0) == 0) {
            handleHeadphone(command);
            return;
        }
        reply("ERR unknown-command\n");
    }

    FireWireDevice* device_ = nullptr;
    unsigned sampleRate_ = 0;
    UInt32 generation_ = 0;
    bool restoringControlState_ = false;
    macfw::fw1814::SpecialMixerRoutingModel routing_{};
    std::array<bool, 4> analogInputMonitorLevelKnown_{{
        false, false, false, false,
    }};
    std::array<std::uint32_t, 4> gainAnalogIn_{{0, 0, 0, 0}};
    int listenFd_ = -1;
    int clientFd_ = -1;
    std::string request_;
};

} // namespace macfw::fw1814::transport
