#pragma once

#include <cstddef>
#include <cstdint>

namespace macfw {

// Non-owning view over interleaved signed PCM samples.
// Samples are carried in int32_t but must fit the signed 24-bit range used by
// AM824 MBLA (-8388608..8388607). AmdtpTransmitRing copies samples while it is
// constructed, so the source buffer does not need to outlive the ring.
struct PcmBufferView {
    const std::int32_t* samples = nullptr;
    std::size_t frameCount = 0;
    std::size_t channelCount = 0;
    bool loop = false;

    bool valid() const {
        return samples != nullptr && frameCount != 0 && channelCount != 0;
    }

    std::int32_t sample(std::size_t frame, std::size_t channel) const {
        if (!valid() || channel >= channelCount)
            return 0;
        if (frame >= frameCount) {
            if (!loop)
                return 0;
            frame %= frameCount;
        }
        return samples[frame * channelCount + channel];
    }
};

} // namespace macfw
