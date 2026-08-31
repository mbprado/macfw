#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace macfw::transport::duplex {

// Software description of the FW410 normal/main mixer.
//
// This class performs no AV/C I/O itself. The FW410 ASIC is known to be
// sensitive to mixer STATUS traffic, and the Linux implementation likewise
// establishes a complete state with CONTROL requests and then trusts cached
// software state rather than polling STATUS.
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
        MixerBus12 = 0,
        MixerBus34,
        MixerBus56,
        MixerBus78,
        MixerBusSpdif,
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

    void clear() { routes_ = {}; }

    // Original/Linux identity-style software-return assignment. The original
    // M-Audio control panel shows these destinations as the five routing
    // buttons at the bottom of each source strip (1/2, 3/4, 5/6, 7/8, spd).
    // They are mixer-bus assignments, not CoreAudio physical-output selectors.
    //
    //   SW Return 1/2   -> Mixer Bus 1/2
    //   SW Return 3/4   -> Mixer Bus 3/4
    //   SW Return 5/6   -> Mixer Bus 5/6
    //   SW Return 7/8   -> Mixer Bus 7/8
    //   SW Return 9/10  -> Mixer Bus S/PDIF
    //
    // This matches snd-firewire-ctl-services' initial cached state. On macfw
    // it is useful as the original logical reference, but it is not the
    // correct hardware baseline for macfw's current AMDTP slot ordering.
    // Software state only: this method sends no AV/C command.
    void loadOriginalIdentityPreset() {
        clear();
        setRoute(Source::SoftwareReturn12, Destination::MixerBus12, true);
        setRoute(Source::SoftwareReturn34, Destination::MixerBus34, true);
        setRoute(Source::SoftwareReturn56, Destination::MixerBus56, true);
        setRoute(Source::SoftwareReturn78, Destination::MixerBus78, true);
        setRoute(Source::SoftwareReturn910, Destination::MixerBusSpdif, true);
    }

    // Hardware-validated macfw routing baseline. The FW410's raw software-
    // return identities are rotated relative to the user-facing CoreAudio
    // playback order used by macfw. Writing all 35 cells with this mapping
    // establishes one coherent mixer state without disturbing normal playback;
    // later route changes can then be issued as differential CONTROL writes
    // against the trusted cached matrix.
    //
    //   raw SW Return 3/4   -> Mixer Bus 1/2
    //   raw SW Return 5/6   -> Mixer Bus 3/4
    //   raw SW Return 7/8   -> Mixer Bus 5/6
    //   raw SW Return 9/10  -> Mixer Bus 7/8
    //   raw SW Return 1/2   -> Mixer Bus S/PDIF
    //
    // Software state only: this method sends no AV/C command.
    void loadMacfwRemappedExperimentPreset() {
        clear();
        setRoute(Source::SoftwareReturn34, Destination::MixerBus12, true);
        setRoute(Source::SoftwareReturn56, Destination::MixerBus34, true);
        setRoute(Source::SoftwareReturn78, Destination::MixerBus56, true);
        setRoute(Source::SoftwareReturn910, Destination::MixerBus78, true);
        setRoute(Source::SoftwareReturn12, Destination::MixerBusSpdif, true);
    }

    // Compatibility aliases for the existing IPC/backend vocabulary. The
    // historical function name is retained to avoid changing tested behavior.
    void loadMacfwPlaybackPreset() { loadMacfwRemappedExperimentPreset(); }

    const RouteMatrix& routes() const { return routes_; }

    static constexpr std::size_t index(Source source) {
        return static_cast<std::size_t>(source);
    }

    static constexpr std::size_t index(Destination destination) {
        return static_cast<std::size_t>(destination);
    }

private:
    // Intentionally starts empty. Hardware state becomes authoritative only
    // after the control server successfully establishes a complete baseline.
    RouteMatrix routes_{};
};

} // namespace macfw::transport::duplex
