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
    std::uint8_t streamPosition; // 1-based AMDTP position
    ChannelKind kind;
    std::uint8_t physicalChannel; // 1-based physical channel; MIDI uses 1
    const char *name;
};

// Device -> host (capture/input) at 48 kHz.
// Physical analog input 1 can be sourced from the front Mic/Inst 1 or rear
// Line Input 1 depending on the FW410's hardware selector; same for input 2.
inline constexpr std::array<StreamChannel, 5> kCapture48k = {{
    {1, ChannelKind::Spdif,  1, "S/PDIF L"},
    {2, ChannelKind::Analog, 1, "Analog Input 1"},
    {3, ChannelKind::Spdif,  2, "S/PDIF R"},
    {4, ChannelKind::Analog, 2, "Analog Input 2"},
    {5, ChannelKind::Midi,   1, "MIDI"},
}};

// Host -> device (playback/output) at 48 kHz.
// This physical mapping was verified by sending a 1 kHz tone to each PCM
// position and identifying the corresponding FW410 output.
inline constexpr std::array<StreamChannel, 11> kPlayback48k = {{
    { 1, ChannelKind::Spdif,  1, "S/PDIF L"},
    { 2, ChannelKind::Analog, 1, "Analog Output 1"},
    { 3, ChannelKind::Analog, 3, "Analog Output 3"},
    { 4, ChannelKind::Analog, 5, "Analog Output 5"},
    { 5, ChannelKind::Analog, 7, "Analog Output 7"},
    { 6, ChannelKind::Spdif,  2, "S/PDIF R"},
    { 7, ChannelKind::Analog, 2, "Analog Output 2"},
    { 8, ChannelKind::Analog, 4, "Analog Output 4"},
    { 9, ChannelKind::Analog, 6, "Analog Output 6"},
    {10, ChannelKind::Analog, 8, "Analog Output 8"},
    {11, ChannelKind::Midi,   1, "MIDI"},
}};

const StreamChannel *captureChannelForPosition(std::size_t position);
const StreamChannel *playbackChannelForPosition(std::size_t position);

} // namespace macfw::fw410
