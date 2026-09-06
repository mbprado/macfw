#pragma once

#include <array>
#include <cstddef>

namespace macfw::fw1814 {

// Hardware-confirmed 48 kHz / S/PDIF-mode stream geometry.
// Digital positions deliberately remain unnamed until later cross-device tests.
inline constexpr std::size_t kCapturePcmPositions = 10;
inline constexpr std::size_t kCaptureMidiPosition = 10;
inline constexpr std::size_t kPlaybackPcmPositions = 6;
inline constexpr std::size_t kPlaybackMidiPosition = 6;

inline constexpr std::size_t kAnalogInputCount = 8;
inline constexpr std::size_t kAnalogOutputCount = 4;

// Physical Analog Input N (array index N-1) -> zero-based PCM position in the
// FW1814's 10-PCM capture portion.
//
// Hardware injection tests:
//   Input 1 -> pos 0
//   Input 2 -> pos 4
//   Input 3 -> pos 1
//   Input 4 -> pos 5
//   Input 5 -> pos 2
//   Input 6 -> pos 6
//   Input 7 -> pos 3
//   Input 8 -> pos 7
inline constexpr std::array<std::size_t, kAnalogInputCount>
    kCapturePositionForAnalogInput{{0, 4, 1, 5, 2, 6, 3, 7}};

// Physical Analog Output N (array index N-1) -> zero-based PCM position in the
// FW1814's 6-PCM playback portion after the documented straight mixer routing
// (MIX_STM_IN=0x00000006, SRC_ANA_OUT=0x00000000) is programmed.
//
// Hardware tone tests:
//   Output 1 <- pos 2
//   Output 2 <- pos 3
//   Output 3 <- pos 0
//   Output 4 <- pos 1
inline constexpr std::array<std::size_t, kAnalogOutputCount>
    kPlaybackPositionForAnalogOutput{{2, 3, 0, 1}};

inline constexpr std::size_t capturePositionForAnalogInput(std::size_t input) {
    return input >= 1 && input <= kAnalogInputCount
        ? kCapturePositionForAnalogInput[input - 1]
        : kCapturePcmPositions;
}

inline constexpr std::size_t playbackPositionForAnalogOutput(std::size_t output) {
    return output >= 1 && output <= kAnalogOutputCount
        ? kPlaybackPositionForAnalogOutput[output - 1]
        : kPlaybackPcmPositions;
}

} // namespace macfw::fw1814
