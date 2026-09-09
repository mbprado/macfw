#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace macfw::fw1814::hal {

constexpr const char* kPlaybackShmName = "/macfw_fw1814_pcm_v1";
constexpr std::uint32_t kPlaybackMagic = 0x4d31384fu; // M18O
constexpr std::uint32_t kPlaybackVersion = 1;
constexpr std::uint32_t kOutputChannels = 4;
constexpr std::uint32_t kCapacityFrames = 32768;

// CoreAudio-facing order is physical and intentionally analog-only for the
// first integrated FW1814 milestone:
//   0 Analog Output 1
//   1 Analog Output 2
//   2 Analog Output 3
//   3 Analog Output 4
// The transport maps these into raw AMDTP positions 2,3,0,1 respectively.
struct SharedPlaybackRing {
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

    std::atomic<std::uint64_t> startIOCalls;
    std::atomic<std::uint64_t> stopIOCalls;
    std::atomic<std::uint64_t> doIOCalls;
    std::atomic<std::uint64_t> doIOFrames;

    float samples[kCapacityFrames * kOutputChannels];
};

inline void initialize(SharedPlaybackRing& ring, std::uint32_t rate = 48000) {
    ring.magic = kPlaybackMagic;
    ring.version = kPlaybackVersion;
    ring.channels = kOutputChannels;
    ring.capacityFrames = kCapacityFrames;
    ring.writeFrame.store(0, std::memory_order_relaxed);
    ring.readFrame.store(0, std::memory_order_relaxed);
    ring.droppedFrames.store(0, std::memory_order_relaxed);
    ring.underrunFrames.store(0, std::memory_order_relaxed);
    ring.sampleRate.store(rate, std::memory_order_relaxed);
    ring.active.store(0, std::memory_order_relaxed);
    ring.startIOCalls.store(0, std::memory_order_relaxed);
    ring.stopIOCalls.store(0, std::memory_order_relaxed);
    ring.doIOCalls.store(0, std::memory_order_relaxed);
    ring.doIOFrames.store(0, std::memory_order_relaxed);
}

inline bool valid(const SharedPlaybackRing& ring) {
    return ring.magic == kPlaybackMagic &&
           ring.version == kPlaybackVersion &&
           ring.channels == kOutputChannels &&
           ring.capacityFrames == kCapacityFrames;
}

inline std::size_t availableFrames(const SharedPlaybackRing& ring) {
    const auto w = ring.writeFrame.load(std::memory_order_acquire);
    const auto r = ring.readFrame.load(std::memory_order_acquire);
    return static_cast<std::size_t>(w - r);
}

inline std::size_t read(SharedPlaybackRing& ring,
                        float* interleaved,
                        std::size_t frames) {
    if (!interleaved || frames == 0) return 0;
    const auto r = ring.readFrame.load(std::memory_order_relaxed);
    const auto w = ring.writeFrame.load(std::memory_order_acquire);
    const std::size_t available = static_cast<std::size_t>(w - r);
    const std::size_t n = frames < available ? frames : available;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t src = static_cast<std::size_t>((r + i) % kCapacityFrames) * kOutputChannels;
        const std::size_t dst = i * kOutputChannels;
        for (std::size_t ch = 0; ch < kOutputChannels; ++ch)
            interleaved[dst + ch] = ring.samples[src + ch];
    }
    ring.readFrame.store(r + n, std::memory_order_release);
    if (n < frames)
        ring.underrunFrames.fetch_add(frames - n, std::memory_order_relaxed);
    return n;
}

inline std::size_t write(SharedPlaybackRing& ring,
                         const float* interleaved,
                         std::size_t frames) {
    if (!interleaved || frames == 0) return 0;
    const auto w = ring.writeFrame.load(std::memory_order_relaxed);
    const auto r = ring.readFrame.load(std::memory_order_acquire);
    const std::size_t used = static_cast<std::size_t>(w - r);
    const std::size_t free = used >= kCapacityFrames ? 0 : kCapacityFrames - used;
    const std::size_t n = frames < free ? frames : free;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t dst = static_cast<std::size_t>((w + i) % kCapacityFrames) * kOutputChannels;
        const std::size_t src = i * kOutputChannels;
        for (std::size_t ch = 0; ch < kOutputChannels; ++ch)
            ring.samples[dst + ch] = interleaved[src + ch];
    }
    ring.writeFrame.store(w + n, std::memory_order_release);
    if (n < frames)
        ring.droppedFrames.fetch_add(frames - n, std::memory_order_relaxed);
    return n;
}

} // namespace macfw::fw1814::hal
