#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace macfw::hal {

constexpr const char* kShmName = "/macfw_fw410_pcm_v3";
constexpr std::uint32_t kMagic = 0x4d465734; // MFW4
constexpr std::uint32_t kVersion = 3;
constexpr std::uint32_t kChannels = 10;
constexpr std::uint32_t kCapacityFrames = 32768;

// CoreAudio-facing channel order. Keep this user-facing/physical rather than
// exposing the FW410's unusual raw AMDTP slot order:
//   0 Analog Out 1
//   1 Analog Out 2
//   2 Analog Out 3
//   3 Analog Out 4
//   4 Analog Out 5
//   5 Analog Out 6
//   6 Analog Out 7
//   7 Analog Out 8
//   8 S/PDIF Out L
//   9 S/PDIF Out R
// The transport layer performs the permutation into AMDTP positions.

struct SharedPcmRing {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t channels;
    std::uint32_t capacityFrames;
    std::atomic<std::uint64_t> writeFrame;
    std::atomic<std::uint64_t> readFrame;
    std::atomic<std::uint64_t> droppedFrames;
    std::atomic<std::uint64_t> underrunFrames;
    std::atomic<std::uint32_t> sampleRate;
    std::atomic<std::uint32_t> active;

    // HAL diagnostic counters. These are intentionally stored in the same
    // shared mapping so shmprobe can inspect the real-time path without
    // logging from CoreAudio's I/O thread.
    std::atomic<std::uint64_t> startIOCalls;
    std::atomic<std::uint64_t> stopIOCalls;
    std::atomic<std::uint64_t> willDoCalls;
    std::atomic<std::uint64_t> beginIOCalls;
    std::atomic<std::uint64_t> doIOCalls;
    std::atomic<std::uint64_t> endIOCalls;
    std::atomic<std::uint64_t> writeMixCalls;
    std::atomic<std::uint64_t> nonNullMainBufferCalls;
    std::atomic<std::uint64_t> doIOFrames;
    std::atomic<std::uint32_t> lastWillDoOperation;
    std::atomic<std::uint32_t> lastBeginOperation;
    std::atomic<std::uint32_t> lastDoOperation;
    std::atomic<std::uint32_t> lastEndOperation;
    std::atomic<std::uint32_t> lastDoFrames;

    float samples[kCapacityFrames * kChannels];
};

inline void initialize(SharedPcmRing& ring, std::uint32_t rate = 44100) {
    ring.magic = kMagic;
    ring.version = kVersion;
    ring.channels = kChannels;
    ring.capacityFrames = kCapacityFrames;
    ring.writeFrame.store(0, std::memory_order_relaxed);
    ring.readFrame.store(0, std::memory_order_relaxed);
    ring.droppedFrames.store(0, std::memory_order_relaxed);
    ring.underrunFrames.store(0, std::memory_order_relaxed);
    ring.sampleRate.store(rate, std::memory_order_relaxed);
    ring.active.store(0, std::memory_order_relaxed);
    ring.startIOCalls.store(0, std::memory_order_relaxed);
    ring.stopIOCalls.store(0, std::memory_order_relaxed);
    ring.willDoCalls.store(0, std::memory_order_relaxed);
    ring.beginIOCalls.store(0, std::memory_order_relaxed);
    ring.doIOCalls.store(0, std::memory_order_relaxed);
    ring.endIOCalls.store(0, std::memory_order_relaxed);
    ring.writeMixCalls.store(0, std::memory_order_relaxed);
    ring.nonNullMainBufferCalls.store(0, std::memory_order_relaxed);
    ring.doIOFrames.store(0, std::memory_order_relaxed);
    ring.lastWillDoOperation.store(0, std::memory_order_relaxed);
    ring.lastBeginOperation.store(0, std::memory_order_relaxed);
    ring.lastDoOperation.store(0, std::memory_order_relaxed);
    ring.lastEndOperation.store(0, std::memory_order_relaxed);
    ring.lastDoFrames.store(0, std::memory_order_relaxed);
}

inline bool valid(const SharedPcmRing& ring) {
    return ring.magic == kMagic && ring.version == kVersion &&
           ring.channels == kChannels && ring.capacityFrames == kCapacityFrames;
}

inline std::size_t availableFrames(const SharedPcmRing& ring) {
    const auto w = ring.writeFrame.load(std::memory_order_acquire);
    const auto r = ring.readFrame.load(std::memory_order_acquire);
    return static_cast<std::size_t>(w - r);
}

inline std::size_t freeFrames(const SharedPcmRing& ring) {
    const std::size_t used = availableFrames(ring);
    return used >= kCapacityFrames ? 0 : kCapacityFrames - used;
}

inline std::size_t write(SharedPcmRing& ring, const float* interleaved, std::size_t frames) {
    if (!interleaved || !frames) return 0;
    const auto w = ring.writeFrame.load(std::memory_order_relaxed);
    const auto r = ring.readFrame.load(std::memory_order_acquire);
    const std::size_t used = static_cast<std::size_t>(w - r);
    const std::size_t free = used >= kCapacityFrames ? 0 : kCapacityFrames - used;
    const std::size_t n = frames < free ? frames : free;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t dst = static_cast<std::size_t>((w + i) % kCapacityFrames) * kChannels;
        const std::size_t src = i * kChannels;
        for (std::size_t ch = 0; ch < kChannels; ++ch)
            ring.samples[dst + ch] = interleaved[src + ch];
    }
    ring.writeFrame.store(w + n, std::memory_order_release);
    if (n < frames) ring.droppedFrames.fetch_add(frames - n, std::memory_order_relaxed);
    return n;
}

inline std::size_t read(SharedPcmRing& ring, float* interleaved, std::size_t frames) {
    if (!interleaved || !frames) return 0;
    const auto r = ring.readFrame.load(std::memory_order_relaxed);
    const auto w = ring.writeFrame.load(std::memory_order_acquire);
    const std::size_t available = static_cast<std::size_t>(w - r);
    const std::size_t n = frames < available ? frames : available;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t src = static_cast<std::size_t>((r + i) % kCapacityFrames) * kChannels;
        const std::size_t dst = i * kChannels;
        for (std::size_t ch = 0; ch < kChannels; ++ch)
            interleaved[dst + ch] = ring.samples[src + ch];
    }
    ring.readFrame.store(r + n, std::memory_order_release);
    if (n < frames) ring.underrunFrames.fetch_add(frames - n, std::memory_order_relaxed);
    return n;
}

} // namespace macfw::hal
