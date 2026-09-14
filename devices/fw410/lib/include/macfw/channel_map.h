#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace macfw::fw410 {

enum class ChannelKind : std::uint8_t {
    Analog,
    Spdif,
    Midi,
};

struct StreamChannel {
    std::uint8_t streamPosition;
    ChannelKind kind;
    std::uint8_t physicalChannel;
    const char *name;
};

inline constexpr std::array<StreamChannel, 5> kCapture48k = {{
    {1, ChannelKind::Spdif,   1, "S/PDIF L"},
    {2, ChannelKind::Analog,  1, "Analog Input 1"},
    {3, ChannelKind::Spdif,   2, "S/PDIF R"},
    {4, ChannelKind::Analog,  2, "Analog Input 2"},
    {5, ChannelKind::Midi,    1, "MIDI"},
}};

inline constexpr std::array<StreamChannel, 11> kPlayback48k = {{
    { 1, ChannelKind::Spdif,   1, "S/PDIF L"},
    { 2, ChannelKind::Analog,  1, "Analog Output 1"},
    { 3, ChannelKind::Analog,  3, "Analog Output 3"},
    { 4, ChannelKind::Analog,  5, "Analog Output 5"},
    { 5, ChannelKind::Analog,  7, "Analog Output 7"},
    { 6, ChannelKind::Spdif,   2, "S/PDIF R"},
    { 7, ChannelKind::Analog,  2, "Analog Output 2"},
    { 8, ChannelKind::Analog,  4, "Analog Output 4"},
    { 9, ChannelKind::Analog,  6, "Analog Output 6"},
    {10, ChannelKind::Analog,  8, "Analog Output 8"},
    {11, ChannelKind::Midi,    1, "MIDI"},
}};

inline constexpr const StreamChannel *captureChannelForPosition(
    std::size_t position) {
    return position >= 1 && position <= kCapture48k.size()
        ? &kCapture48k[position - 1]
        : nullptr;
}

inline constexpr const StreamChannel *playbackChannelForPosition(
    std::size_t position) {
    return position >= 1 && position <= kPlayback48k.size()
        ? &kPlayback48k[position - 1]
        : nullptr;
}

inline constexpr const StreamChannel *playbackAnalogOutput(
    std::size_t output) {
    for (const auto& channel : kPlayback48k) {
        if (channel.kind == ChannelKind::Analog &&
            channel.physicalChannel == output)
            return &channel;
    }
    return nullptr;
}

} // namespace macfw::fw410
