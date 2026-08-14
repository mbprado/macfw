#include "macfw/pcm_ring_buffer.h"

#include <algorithm>
#include <cstring>

namespace macfw {

PcmRingBuffer::PcmRingBuffer(std::size_t capacityFrames, std::size_t channelCount)
    : capacityFrames_(capacityFrames),
      channelCount_(channelCount),
      storage_(capacityFrames * channelCount, 0) {
    if (capacityFrames == 0 || channelCount == 0) {
        capacityFrames_ = 0;
        channelCount_ = 0;
        storage_.clear();
    }
}

std::size_t PcmRingBuffer::availableFrames() const {
    const auto write = writeFrame_.load(std::memory_order_acquire);
    const auto read = readFrame_.load(std::memory_order_acquire);
    const auto available = write - read;
    return static_cast<std::size_t>(std::min<std::uint64_t>(available, capacityFrames_));
}

std::size_t PcmRingBuffer::freeFrames() const {
    return valid() ? capacityFrames_ - availableFrames() : 0;
}

std::size_t PcmRingBuffer::write(const std::int32_t* interleaved, std::size_t frames) {
    if (!valid() || !interleaved || frames == 0)
        return 0;

    const auto read = readFrame_.load(std::memory_order_acquire);
    const auto write = writeFrame_.load(std::memory_order_relaxed);
    const std::size_t used = static_cast<std::size_t>(write - read);
    const std::size_t free = used < capacityFrames_ ? capacityFrames_ - used : 0;
    const std::size_t accepted = std::min(frames, free);

    for (std::size_t frame = 0; frame < accepted; ++frame) {
        const std::size_t dstFrame = static_cast<std::size_t>((write + frame) % capacityFrames_);
        std::memcpy(storage_.data() + dstFrame * channelCount_,
                    interleaved + frame * channelCount_,
                    channelCount_ * sizeof(std::int32_t));
    }

    writeFrame_.store(write + accepted, std::memory_order_release);
    return accepted;
}

PcmRingBuffer::ReadResult PcmRingBuffer::read(std::int32_t* dstInterleaved,
                                               std::size_t frames) {
    ReadResult result{frames, 0, frames};
    if (!dstInterleaved || frames == 0)
        return result;

    std::fill(dstInterleaved, dstInterleaved + frames * channelCount_, 0);
    if (!valid()) {
        underrunFrames_.fetch_add(frames, std::memory_order_relaxed);
        return result;
    }

    const auto write = writeFrame_.load(std::memory_order_acquire);
    const auto read = readFrame_.load(std::memory_order_relaxed);
    const std::size_t available = static_cast<std::size_t>(write - read);
    const std::size_t consumed = std::min(frames, available);

    for (std::size_t frame = 0; frame < consumed; ++frame) {
        const std::size_t srcFrame = static_cast<std::size_t>((read + frame) % capacityFrames_);
        std::memcpy(dstInterleaved + frame * channelCount_,
                    storage_.data() + srcFrame * channelCount_,
                    channelCount_ * sizeof(std::int32_t));
    }

    readFrame_.store(read + consumed, std::memory_order_release);
    result.framesFromBuffer = consumed;
    result.framesSilenced = frames - consumed;
    if (result.framesSilenced)
        underrunFrames_.fetch_add(result.framesSilenced, std::memory_order_relaxed);
    return result;
}

void PcmRingBuffer::reset() {
    writeFrame_.store(0, std::memory_order_release);
    readFrame_.store(0, std::memory_order_release);
    underrunFrames_.store(0, std::memory_order_release);
    std::fill(storage_.begin(), storage_.end(), 0);
}

} // namespace macfw
