#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace macfw::hal {

constexpr const char* kShmName = "/macfw_fw410_pcm_v1";
constexpr std::uint32_t kMagic = 0x4d465734; // MFW4
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kCapacityFrames = 32768;

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
        ring.samples[dst] = interleaved[i * kChannels];
        ring.samples[dst + 1] = interleaved[i * kChannels + 1];
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
        interleaved[i * kChannels] = ring.samples[src];
        interleaved[i * kChannels + 1] = ring.samples[src + 1];
    }
    ring.readFrame.store(r + n, std::memory_order_release);
    if (n < frames) ring.underrunFrames.fetch_add(frames - n, std::memory_order_relaxed);
    return n;
}

} // namespace macfw::hal
