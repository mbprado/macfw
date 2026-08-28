#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace macfw::transport::duplex {

// Software-only description of the FW410 normal/main mixer.
//
// This deliberately performs no AV/C I/O.  The FW410 ASIC is known to be
// sensitive to mixer STATUS traffic, and the Linux implementation does not
// use STATUS to discover the normal mixer state.  Keep topology/state separate
// from hardware programming until the complete model is validated.
class Fw410MainMixerModel {
public:
    static constexpr std::size_t kSourceCount = 7;
    static constexpr std::size_t kDestinationCount = 5;

    enum class Source : std::size_t {
        AnalogInput12 = 0,
        SpdifInputLR,
        SoftwareReturn12,
        SoftwareReturn34,
        SoftwareReturn56,
        SoftwareReturn78,
        SoftwareReturn910,
    };

    enum class Destination : std::size_t {
        MixerOutput12 = 0,
        MixerOutput34,
        MixerOutput56,
        MixerOutput78,
        MixerOutput910,
    };

    struct AvcSource {
        std::uint8_t functionBlock;
        std::uint8_t channel;
    };

    // Mapping from snd-firewire-ctl-services Fw410MixerProtocol.
    // AudioCh::Each(n) serializes as n + 1 for these AV/C processing mixer
    // operands, hence stream pairs 3/4, 5/6, 7/8, 9/10 use 1,3,5,7.
    static constexpr std::array<AvcSource, kSourceCount> kAvcSources{{
        {0x02, 0x01}, // Analog input 1/2
        {0x03, 0x01}, // S/PDIF input L/R
        {0x01, 0x01}, // Software return 1/2
        {0x00, 0x01}, // Software return 3/4
        {0x00, 0x03}, // Software return 5/6
        {0x00, 0x05}, // Software return 7/8
        {0x00, 0x07}, // Software return 9/10
    }};

    static constexpr std::uint8_t kDestinationFunctionBlock = 0x01;
    static constexpr std::array<std::uint8_t, kDestinationCount> kAvcDestinationChannels{{
        0x01, 0x03, 0x05, 0x07, 0x09
    }};

    using RouteRow = std::array<bool, kSourceCount>;
    using RouteMatrix = std::array<RouteRow, kDestinationCount>;

    Fw410MainMixerModel() = default;

    bool route(Source source, Destination destination) const {
        return routes_[index(destination)][index(source)];
    }

    void setRoute(Source source, Destination destination, bool enabled) {
        routes_[index(destination)][index(source)] = enabled;
    }

    const RouteMatrix& routes() const { return routes_; }

    static constexpr std::size_t index(Source source) {
        return static_cast<std::size_t>(source);
    }

    static constexpr std::size_t index(Destination destination) {
        return static_cast<std::size_t>(destination);
    }

private:
    // Intentionally starts empty.  This is software state only and must not be
    // presented as the current hardware state.  A future initialization policy
    // can populate it only after we decide exactly which complete matrix macfw
    // should program into the device.
    RouteMatrix routes_{};
};

} // namespace macfw::transport::duplex
