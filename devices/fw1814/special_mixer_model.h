#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace macfw::fw1814 {

// FFADO-documented values used by the hardware-validated macfw analog
// playback baseline. The FW1814 special mixer registers are write-only, so
// every runtime update must be derived from this authoritative software state.
inline constexpr std::uint32_t kStraightStreamToMixer = 0x00000006u;
inline constexpr std::uint32_t kAnalogFromMixers = 0x00000000u;

class SpecialMixerRoutingModel {
public:
    static constexpr std::size_t kStreamSourceCount = 2;
    static constexpr std::size_t kMixerBusCount = 2;
    static constexpr std::size_t kAnalogOutputPairCount = 2;

    enum class StreamSource : std::size_t {
        Stream12 = 0,
        Stream34,
    };

    enum class MixerBus : std::size_t {
        Mixer12 = 0,
        Mixer34,
    };

    enum class AnalogOutputPair : std::size_t {
        Analog12 = 0,
        Analog34,
    };

    enum class OutputSource : std::uint8_t {
        Mixer = 0,
        Aux = 1,
    };

    // MIX_STM_IN bit layout documented by FFADO:
    //   bit 3: Stream 1/2 -> Mixer 3/4
    //   bit 2: Stream 1/2 -> Mixer 1/2
    //   bit 1: Stream 3/4 -> Mixer 3/4
    //   bit 0: Stream 3/4 -> Mixer 1/2
    inline static constexpr std::array<std::array<std::uint32_t,
                                                  kMixerBusCount>,
                                       kStreamSourceCount>
        kStreamRouteMasks{{
            {{0x04u, 0x08u}},
            {{0x01u, 0x02u}},
        }};

    void loadStraightAnalogPlaybackPreset() {
        mixStreamIn_ = kStraightStreamToMixer;
        srcAnalogOut_ = kAnalogFromMixers;
    }

    bool isStraightAnalogPlaybackPreset() const {
        return mixStreamIn_ == kStraightStreamToMixer &&
               srcAnalogOut_ == kAnalogFromMixers;
    }

    bool streamRoute(StreamSource source, MixerBus destination) const {
        return (mixStreamIn_ & routeMask(source, destination)) != 0;
    }

    void setStreamRoute(StreamSource source,
                        MixerBus destination,
                        bool enabled) {
        const std::uint32_t mask = routeMask(source, destination);
        if (enabled)
            mixStreamIn_ |= mask;
        else
            mixStreamIn_ &= ~mask;
        mixStreamIn_ &= 0x0fu;
    }

    OutputSource analogOutputSource(AnalogOutputPair pair) const {
        const auto mask = static_cast<std::uint32_t>(1u << index(pair));
        return (srcAnalogOut_ & mask) != 0
            ? OutputSource::Aux
            : OutputSource::Mixer;
    }

    void setAnalogOutputSource(AnalogOutputPair pair, OutputSource source) {
        const auto mask = static_cast<std::uint32_t>(1u << index(pair));
        if (source == OutputSource::Aux)
            srcAnalogOut_ |= mask;
        else
            srcAnalogOut_ &= ~mask;
        srcAnalogOut_ &= 0x03u;
    }

    std::uint32_t mixStreamIn() const { return mixStreamIn_; }
    std::uint32_t srcAnalogOut() const { return srcAnalogOut_; }

    static constexpr std::size_t index(StreamSource source) {
        return static_cast<std::size_t>(source);
    }

    static constexpr std::size_t index(MixerBus destination) {
        return static_cast<std::size_t>(destination);
    }

    static constexpr std::size_t index(AnalogOutputPair pair) {
        return static_cast<std::size_t>(pair);
    }

private:
    static constexpr std::uint32_t routeMask(StreamSource source,
                                             MixerBus destination) {
        return kStreamRouteMasks[index(source)][index(destination)];
    }

    std::uint32_t mixStreamIn_ = kStraightStreamToMixer;
    std::uint32_t srcAnalogOut_ = kAnalogFromMixers;
};

} // namespace macfw::fw1814
