#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace macfw::hal::capture {

constexpr const char* kShmName = "/macfw_fw410_capture_v1";
constexpr std::uint32_t kMagic = 0x4d464349; // MFCI
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kChannels = 4;
constexpr std::uint32_t kCapacityFrames = 32768;

// CoreAudio-facing input order:
//   0 Analog Input 1
//   1 Analog Input 2
//   2 S/PDIF Input L
//   3 S/PDIF Input R
struct SharedCaptureRing {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t channels;
    std::uint32_t capacityFrames;
    std::atomic<std::uint64_t> writeFrame;
    std::atomic<std::uint64_t> readFrame;
    std::atomic<std::uint64_t> droppedFrames;
    std::atomic<std::uint32_t> sampleRate;
    std::atomic<std::uint32_t> active;
    std::atomic<std::uint64_t> decodedPackets;
    std::atomic<std::uint64_t> decodedFrames;
    std::atomic<std::uint64_t> malformedPackets;
    std::atomic<std::uint64_t> invalidLabels;
    float samples[kCapacityFrames * kChannels];
};

inline void initialize(SharedCaptureRing& ring, std::uint32_t rate) {
    ring.magic = kMagic;
    ring.version = kVersion;
    ring.channels = kChannels;
    ring.capacityFrames = kCapacityFrames;
    ring.writeFrame.store(0, std::memory_order_relaxed);
    ring.readFrame.store(0, std::memory_order_relaxed);
    ring.droppedFrames.store(0, std::memory_order_relaxed);
    ring.sampleRate.store(rate, std::memory_order_relaxed);
    ring.active.store(0, std::memory_order_relaxed);
    ring.decodedPackets.store(0, std::memory_order_relaxed);
    ring.decodedFrames.store(0, std::memory_order_relaxed);
    ring.malformedPackets.store(0, std::memory_order_relaxed);
    ring.invalidLabels.store(0, std::memory_order_relaxed);
}

inline bool valid(const SharedCaptureRing& ring) {
    return ring.magic == kMagic && ring.version == kVersion &&
           ring.channels == kChannels && ring.capacityFrames == kCapacityFrames;
}

inline std::size_t availableFrames(const SharedCaptureRing& ring) {
    const auto w = ring.writeFrame.load(std::memory_order_acquire);
    const auto r = ring.readFrame.load(std::memory_order_acquire);
    return static_cast<std::size_t>(w - r);
}

inline std::size_t write(SharedCaptureRing& ring, const float* interleaved, std::size_t frames) {
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

inline std::size_t read(SharedCaptureRing& ring, float* interleaved, std::size_t frames) {
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
    return n;
}

} // namespace macfw::hal::capture
