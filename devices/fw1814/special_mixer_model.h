#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace macfw::fw1814 {

// FFADO-documented values used by the hardware-validated macfw analog
// playback baseline. The FW1814 special mixer registers are write-only, so
// every runtime update must be derived from this authoritative software state.
inline constexpr std::uint32_t kStraightStreamToMixer = 0x00000006u;
inline constexpr std::uint32_t kAnalogInputsMuted = 0x00000000u;
inline constexpr std::uint32_t kAnalogFromMixers = 0x00000000u;
inline constexpr std::uint32_t kHeadphonesFromMixer12 = 0x00010001u;
inline constexpr std::uint16_t kMonitorLevelMute = 0x8000u;
inline constexpr std::uint16_t kMonitorLevelUnity = 0x0000u;
// AV/C volume values use signed 8.8 dB units. -20 dB is -20 * 256.
inline constexpr std::uint16_t kMonitorLevelMinus20Db = 0xec00u;
inline constexpr std::uint16_t kPanHardRight = 0x8000u;
inline constexpr std::uint16_t kPanHalfRight = 0xc000u;
inline constexpr std::uint16_t kPanCenter = 0x0000u;
inline constexpr std::uint16_t kPanHalfLeft = 0x4000u;
inline constexpr std::uint16_t kPanHardLeft = 0x7ffeu;
inline constexpr std::uint32_t kAnalogInputPanBaseline = 0x7ffe8000u;

inline constexpr std::uint32_t stereoMonitorLevelWord(
    std::uint16_t level) {
    return (static_cast<std::uint32_t>(level) << 16) | level;
}

inline constexpr std::uint16_t monitorLevelChannel(
    std::uint32_t word, std::size_t channel) {
    return channel == 0
        ? static_cast<std::uint16_t>(word >> 16)
        : static_cast<std::uint16_t>(word & 0xffffu);
}

inline constexpr std::uint32_t setMonitorLevelChannel(
    std::uint32_t word, std::size_t channel, std::uint16_t level) {
    return channel == 0
        ? (word & 0x0000ffffu) | (static_cast<std::uint32_t>(level) << 16)
        : (word & 0xffff0000u) | level;
}

// Monitor gain uses signed AV/C 8.8 dB units.  The production CLI follows
// fw410ctl and exposes whole dB steps from -128 through 0; -128 is the AV/C
// negative-infinity/mute endpoint.
inline constexpr std::uint16_t monitorLevelFromDb(int db) {
    return db <= -128
        ? kMonitorLevelMute
        : static_cast<std::uint16_t>(db * 0x100);
}

inline constexpr int monitorLevelRaw(std::uint16_t level) {
    return level <= 0x7fffu
        ? static_cast<int>(level)
        : static_cast<int>(level) - 0x10000;
}

inline constexpr int monitorLevelDb(std::uint16_t level) {
    return monitorLevelRaw(level) / 0x100;
}

inline constexpr std::uint16_t inputPanChannel(
    std::uint32_t word, std::size_t channel) {
    return monitorLevelChannel(word, channel);
}

inline constexpr std::uint32_t setInputPanChannel(
    std::uint32_t word, std::size_t channel, std::uint16_t pan) {
    return setMonitorLevelChannel(word, channel, pan);
}

// User-facing pan is normalized to -100 (left), 0 (center), +100 (right).
// The hardware uses the opposite signed direction and reserves +32767, so
// hard left is the documented +32766 endpoint.
inline constexpr std::uint16_t inputPanFromPercent(int percent) {
    if (percent <= -100) return kPanHardLeft;
    if (percent >= 100) return kPanHardRight;
    if (percent == 0) return kPanCenter;
    const int magnitude =
        ((percent < 0 ? -percent : percent) * 32768 + 50) / 100;
    return static_cast<std::uint16_t>(percent < 0 ? magnitude : -magnitude);
}

inline constexpr int inputPanPercent(std::uint16_t pan) {
    const int signedPan = pan <= 0x7fffu
        ? static_cast<int>(pan)
        : static_cast<int>(pan) - 0x10000;
    if (signedPan == 0) return 0;
    const int magnitude = signedPan < 0 ? -signedPan : signedPan;
    const int percent = (magnitude * 100 + 16384) / 32768;
    return signedPan > 0 ? -percent : percent;
}

enum class HeadphoneSource : std::uint32_t {
    Mixer12 = 0x01u,
    Mixer34 = 0x02u,
    Aux12 = 0x04u,
};

inline constexpr std::uint32_t headphoneSourceWord(
    HeadphoneSource headphone12,
    HeadphoneSource headphone34) {
    return static_cast<std::uint32_t>(headphone12) |
           (static_cast<std::uint32_t>(headphone34) << 16);
}

class SpecialMixerRoutingModel {
public:
    static constexpr std::size_t kStreamSourceCount = 2;
    static constexpr std::size_t kMixerBusCount = 2;
    static constexpr std::size_t kAnalogInputPairCount = 4;
    static constexpr std::size_t kAnalogOutputPairCount = 2;
    static constexpr std::size_t kHeadphoneOutputCount = 2;

    enum class StreamSource : std::size_t {
        Stream12 = 0,
        Stream34,
    };

    enum class MixerBus : std::size_t {
        Mixer12 = 0,
        Mixer34,
    };

    enum class AnalogInputPair : std::size_t {
        Analog12 = 0,
        Analog34,
        Analog56,
        Analog78,
    };

    enum class AnalogOutputPair : std::size_t {
        Analog12 = 0,
        Analog34,
    };

    enum class OutputSource : std::uint8_t {
        Mixer = 0,
        Aux = 1,
    };

    enum class HeadphoneOutput : std::size_t {
        Output1 = 0,
        Output2,
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

    // MIX_ANA_DIG_IN low-byte layout documented by FFADO:
    //   bits 0..3: Analog 1/2, 3/4, 5/6, 7/8 -> Mixer 1/2
    //   bits 4..7: Analog 1/2, 3/4, 5/6, 7/8 -> Mixer 3/4
    inline static constexpr std::array<std::array<std::uint32_t,
                                                  kMixerBusCount>,
                                       kAnalogInputPairCount>
        kAnalogInputRouteMasks{{
            {{0x01u, 0x10u}},
            {{0x02u, 0x20u}},
            {{0x04u, 0x40u}},
            {{0x08u, 0x80u}},
        }};

    void loadStraightAnalogPlaybackPreset() {
        mixStreamIn_ = kStraightStreamToMixer;
        mixAnalogDigitalIn_ = kAnalogInputsMuted;
        srcHeadphoneOut_ = kHeadphonesFromMixer12;
        srcAnalogOut_ = kAnalogFromMixers;
    }

    bool isStraightAnalogPlaybackPreset() const {
        return mixStreamIn_ == kStraightStreamToMixer &&
               mixAnalogDigitalIn_ == kAnalogInputsMuted &&
               srcHeadphoneOut_ == kHeadphonesFromMixer12 &&
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

    bool analogInputRoute(AnalogInputPair source,
                          MixerBus destination) const {
        return (mixAnalogDigitalIn_ & routeMask(source, destination)) != 0;
    }

    void setAnalogInputRoute(AnalogInputPair source,
                             MixerBus destination,
                             bool enabled) {
        const std::uint32_t mask = routeMask(source, destination);
        if (enabled)
            mixAnalogDigitalIn_ |= mask;
        else
            mixAnalogDigitalIn_ &= ~mask;
        mixAnalogDigitalIn_ &= 0xffu;
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

    HeadphoneSource headphoneSource(HeadphoneOutput output) const {
        const std::size_t shift = index(output) * 16;
        const std::uint32_t value = (srcHeadphoneOut_ >> shift) & 0xffffu;
        return value & static_cast<std::uint32_t>(HeadphoneSource::Aux12)
            ? HeadphoneSource::Aux12
            : value & static_cast<std::uint32_t>(HeadphoneSource::Mixer34)
                ? HeadphoneSource::Mixer34
                : HeadphoneSource::Mixer12;
    }

    void setHeadphoneSource(HeadphoneOutput output, HeadphoneSource source) {
        const std::size_t shift = index(output) * 16;
        const std::uint32_t mask = 0xffffu << shift;
        srcHeadphoneOut_ =
            (srcHeadphoneOut_ & ~mask) |
            (static_cast<std::uint32_t>(source) << shift);
    }

    std::uint32_t mixStreamIn() const { return mixStreamIn_; }
    std::uint32_t mixAnalogDigitalIn() const { return mixAnalogDigitalIn_; }
    std::uint32_t srcHeadphoneOut() const { return srcHeadphoneOut_; }
    std::uint32_t srcAnalogOut() const { return srcAnalogOut_; }

    static constexpr std::size_t index(StreamSource source) {
        return static_cast<std::size_t>(source);
    }

    static constexpr std::size_t index(MixerBus destination) {
        return static_cast<std::size_t>(destination);
    }

    static constexpr std::size_t index(AnalogInputPair pair) {
        return static_cast<std::size_t>(pair);
    }

    static constexpr std::size_t index(AnalogOutputPair pair) {
        return static_cast<std::size_t>(pair);
    }

    static constexpr std::size_t index(HeadphoneOutput output) {
        return static_cast<std::size_t>(output);
    }

private:
    static constexpr std::uint32_t routeMask(StreamSource source,
                                             MixerBus destination) {
        return kStreamRouteMasks[index(source)][index(destination)];
    }

    static constexpr std::uint32_t routeMask(AnalogInputPair source,
                                             MixerBus destination) {
        return kAnalogInputRouteMasks[index(source)][index(destination)];
    }

    std::uint32_t mixStreamIn_ = kStraightStreamToMixer;
    std::uint32_t mixAnalogDigitalIn_ = kAnalogInputsMuted;
    std::uint32_t srcHeadphoneOut_ = kHeadphonesFromMixer12;
    std::uint32_t srcAnalogOut_ = kAnalogFromMixers;
};

} // namespace macfw::fw1814
