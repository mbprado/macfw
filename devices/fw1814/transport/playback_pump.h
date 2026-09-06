#pragma once

#include "../channel_map.h"
#include "../hal/include/macfw_fw1814_hal_shm.h"
#include "macfw/pcm_ring_buffer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace macfw::fw1814::transport {

inline std::size_t pumpPlayback(
    macfw::fw1814::hal::SharedPlaybackRing& shared,
    macfw::PcmRingBuffer& pcm,
    std::vector<float>& audio,
    std::vector<std::int32_t>& mapped) {
    const std::size_t frames = std::min<std::size_t>({
        pcm.freeFrames(),
        macfw::fw1814::hal::availableFrames(shared),
        audio.size() / macfw::fw1814::hal::kOutputChannels
    });
    if (frames == 0) return 0;

    const std::size_t got =
        macfw::fw1814::hal::read(shared, audio.data(), frames);
    for (std::size_t frame = 0; frame < got; ++frame) {
        const std::size_t outBase = frame * kPlaybackPcmPositions;
        std::fill_n(mapped.data() + outBase, kPlaybackPcmPositions, 0);

        for (std::size_t physical = 0;
             physical < macfw::fw1814::hal::kOutputChannels;
             ++physical) {
            const double sample = std::max(
                -1.0, std::min(1.0, static_cast<double>(
                    audio[frame * macfw::fw1814::hal::kOutputChannels + physical])));
            const std::size_t position =
                kPlaybackPositionForAnalogOutput[physical];
            mapped[outBase + position] =
                static_cast<std::int32_t>(sample * 8388607.0);
        }
        // Raw PCM positions 4/5 are the verified S/PDIF pair, but stay zero
        // until their L/R orientation is proven by the later cross-device test.
    }
    return pcm.write(mapped.data(), got);
}

inline void drainPlayback(
    macfw::fw1814::hal::SharedPlaybackRing& shared,
    macfw::PcmRingBuffer& pcm,
    std::vector<float>& audio,
    std::vector<std::int32_t>& mapped) {
    while (macfw::fw1814::hal::availableFrames(shared) != 0 &&
           pcm.freeFrames() != 0) {
        if (pumpPlayback(shared, pcm, audio, mapped) == 0)
            break;
    }
}

} // namespace macfw::fw1814::transport
