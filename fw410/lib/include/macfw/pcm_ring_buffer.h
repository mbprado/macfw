#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace macfw {

class PcmRingBuffer {
public:
    struct ReadResult {
        std::size_t framesRequested = 0;
        std::size_t framesFromBuffer = 0;
        std::size_t framesSilenced = 0;
    };

    PcmRingBuffer() = default;
    PcmRingBuffer(std::size_t capacityFrames, std::size_t channelCount);

    bool valid() const { return capacityFrames_ != 0 && channelCount_ != 0 && !storage_.empty(); }
    std::size_t capacityFrames() const { return capacityFrames_; }
    std::size_t channelCount() const { return channelCount_; }

    std::size_t availableFrames() const;
    std::size_t freeFrames() const;

    // Single-producer side. Returns the number of complete frames accepted.
    std::size_t write(const std::int32_t* interleaved, std::size_t frames);

    // Single-consumer side. Always writes exactly 'frames' frames to dst.
    // If the ring underruns, unavailable frames are filled with digital zero.
    ReadResult read(std::int32_t* dstInterleaved, std::size_t frames);

    std::uint64_t producedFrames() const { return writeFrame_.load(std::memory_order_acquire); }
    std::uint64_t consumedFrames() const { return readFrame_.load(std::memory_order_acquire); }
    std::uint64_t underrunFrames() const { return underrunFrames_.load(std::memory_order_acquire); }

    void reset();

private:
    std::size_t capacityFrames_ = 0;
    std::size_t channelCount_ = 0;
    std::vector<std::int32_t> storage_;
    std::atomic<std::uint64_t> writeFrame_{0};
    std::atomic<std::uint64_t> readFrame_{0};
    std::atomic<std::uint64_t> underrunFrames_{0};
};

} // namespace macfw
