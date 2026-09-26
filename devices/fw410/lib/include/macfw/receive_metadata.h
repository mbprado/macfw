#pragma once

#include <cstddef>
#include <cstdint>

namespace macfw {

struct ReceiveMetadata {
    std::uint32_t isoHeader;
    std::uint32_t status;
    std::uint32_t timestamp;
    bool byteSwapped = false;
};

inline std::uint32_t byteSwap32(std::uint32_t value) {
    return ((value & 0x000000ffu) << 24) |
           ((value & 0x0000ff00u) << 8) |
           ((value & 0x00ff0000u) >> 8) |
           ((value & 0xff000000u) >> 24);
}

// On the test Mac, some receive DCL completions publish all three metadata
// words with reversed byte order while the CIP payload remains unchanged.
// Only correct the observed completion status and an otherwise impossible
// length; never infer packet length from payload bytes alone.
inline ReceiveMetadata normalizeReceiveMetadata(
    std::uint32_t header, std::uint32_t status, std::uint32_t timestamp,
    std::size_t capacity) {
    ReceiveMetadata result{header, status, timestamp, false};
    const auto swappedHeader = byteSwap32(header);
    const auto swappedLength = swappedHeader >> 16;
    if ((header >> 16) > capacity &&
        byteSwap32(status) == 0x00008451u &&
        swappedLength >= 8 && swappedLength <= capacity &&
        (swappedLength % 4) == 0) {
        result = {swappedHeader, byteSwap32(status),
                  byteSwap32(timestamp), true};
    }
    return result;
}

} // namespace macfw
