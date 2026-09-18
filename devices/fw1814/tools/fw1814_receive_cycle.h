#pragma once

#include <cstdint>

namespace macfw::fw1814::experimental {

// The receive timestamp contains a FireWire cycle in bits 12..24. Its
// seconds field can wrap independently of the 32-bit integer; compare cycles
// within the 32-ms receive ring rather than comparing the whole timestamp.
constexpr std::uint32_t receiveCycle(std::uint32_t timestamp) {
    return (timestamp >> 12) & 0x1fffu;
}

constexpr std::uint32_t forwardCycleDistance(std::uint32_t newer,
                                              std::uint32_t older) {
    return (receiveCycle(newer) + 8000u - receiveCycle(older)) % 8000u;
}

// This ordering is valid for observations less than half a second apart.
// The capture ring spans 32 ms and the engine services it every 250 us.
constexpr bool receiveTimestampAfter(std::uint32_t newer,
                                      std::uint32_t older) {
    const auto delta = forwardCycleDistance(newer, older);
    return delta != 0 && delta < 4000u;
}

} // namespace macfw::fw1814::experimental
