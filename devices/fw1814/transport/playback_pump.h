#pragma once

#include "../channel_map.h"
#include "../hal/include/macfw_fw1814_hal_shm.h"
#include "macfw/pcm_ring_buffer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace macfw::fw1814::transport {

struct PlaybackPumpStats {
    std::uint64_t framesRead = 0;
    std::uint64_t samplesSeen = 0;
    std::uint64_t clippedSamples = 0;
    std::uint64_t nonFiniteSamples = 0;
    double peakAbs = 0.0;
};

inline std::size_t pumpPlayback(
    macfw::fw1814::hal::SharedPlaybackRing& shared,
    macfw::PcmRingBuffer& pcm,
    std::vector<float>& audio,
    std::vector<std::int32_t>& mapped,
    PlaybackPumpStats* stats = nullptr) {
    const std::size_t frames = std::min<std::size_t>({
        pcm.freeFrames(),
        macfw::fw1814::hal::availableFrames(shared),
        audio.size() / macfw::fw1814::hal::kOutputChannels
    });
    if (frames == 0) return 0;

    const std::size_t got =
        macfw::fw1814::hal::read(shared, audio.data(), frames);
    if (stats)
        stats->framesRead += got;

    for (std::size_t frame = 0; frame < got; ++frame) {
        const std::size_t outBase = frame * kPlaybackPcmPositions;
        std::fill_n(mapped.data() + outBase, kPlaybackPcmPositions, 0);

        for (std::size_t physical = 0;
             physical < macfw::fw1814::hal::kOutputChannels;
             ++physical) {
            const float rawFloat =
                audio[frame * macfw::fw1814::hal::kOutputChannels + physical];
            double raw = static_cast<double>(rawFloat);
            if (stats) {
                ++stats->samplesSeen;
                if (!std::isfinite(raw)) {
                    ++stats->nonFiniteSamples;
                } else {
                    stats->peakAbs = std::max(stats->peakAbs, std::fabs(raw));
                    if (raw < -1.0 || raw > 1.0)
                        ++stats->clippedSamples;
                }
            }
            if (!std::isfinite(raw)) raw = 0.0;
            const double sample = std::max(-1.0, std::min(1.0, raw));
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
    std::vector<std::int32_t>& mapped,
    PlaybackPumpStats* stats = nullptr) {
    while (macfw::fw1814::hal::availableFrames(shared) != 0 &&
           pcm.freeFrames() != 0) {
        if (pumpPlayback(shared, pcm, audio, mapped, stats) == 0)
            break;
    }
}

} // namespace macfw::fw1814::transport
