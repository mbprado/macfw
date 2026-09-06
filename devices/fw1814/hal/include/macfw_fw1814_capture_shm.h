#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace macfw::fw1814::hal::capture {

constexpr const char* kShmName = "/macfw_fw1814_capture_v1";
constexpr std::uint32_t kMagic = 0x4d313849u; // M18I
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kInputChannels = 8;
constexpr std::uint32_t kCapacityFrames = 32768;
constexpr std::uint64_t kQueueMinUnset = std::numeric_limits<std::uint64_t>::max();

// CoreAudio-facing order is physical and analog-only:
//   0..7 = Analog Inputs 1..8
// Raw capture positions 8/9 and MIDI position 10 stay hidden until verified.
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

    std::atomic<std::uint64_t> halReadCalls;
    std::atomic<std::uint64_t> halRequestedFrames;
    std::atomic<std::uint64_t> halFramesFromRing;
    std::atomic<std::uint64_t> halZeroFilledFrames;
    std::atomic<std::uint64_t> halMinQueuedFrames;
    std::atomic<std::uint64_t> halMaxQueuedFrames;
    std::atomic<std::uint64_t> halUnderrunEvents;

    float samples[kCapacityFrames * kInputChannels];
};

inline void initialize(SharedCaptureRing& ring, std::uint32_t rate = 48000) {
    ring.magic = kMagic;
    ring.version = kVersion;
    ring.channels = kInputChannels;
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
    ring.halReadCalls.store(0, std::memory_order_relaxed);
    ring.halRequestedFrames.store(0, std::memory_order_relaxed);
    ring.halFramesFromRing.store(0, std::memory_order_relaxed);
    ring.halZeroFilledFrames.store(0, std::memory_order_relaxed);
    ring.halMinQueuedFrames.store(kQueueMinUnset, std::memory_order_relaxed);
    ring.halMaxQueuedFrames.store(0, std::memory_order_relaxed);
    ring.halUnderrunEvents.store(0, std::memory_order_relaxed);
}

inline bool valid(const SharedCaptureRing& ring) {
    return ring.magic == kMagic && ring.version == kVersion &&
           ring.channels == kInputChannels &&
           ring.capacityFrames == kCapacityFrames;
}

inline std::size_t availableFrames(const SharedCaptureRing& ring) {
    const auto w = ring.writeFrame.load(std::memory_order_acquire);
    const auto r = ring.readFrame.load(std::memory_order_acquire);
    return static_cast<std::size_t>(w - r);
}

inline void observeQueueDepth(SharedCaptureRing& ring, std::uint64_t queued) {
    auto minQueued = ring.halMinQueuedFrames.load(std::memory_order_relaxed);
    while (queued < minQueued &&
           !ring.halMinQueuedFrames.compare_exchange_weak(
               minQueued, queued,
               std::memory_order_relaxed, std::memory_order_relaxed)) {}

    auto maxQueued = ring.halMaxQueuedFrames.load(std::memory_order_relaxed);
    while (queued > maxQueued &&
           !ring.halMaxQueuedFrames.compare_exchange_weak(
               maxQueued, queued,
               std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

inline std::size_t write(SharedCaptureRing& ring,
                         const float* interleaved,
                         std::size_t frames) {
    if (!interleaved || frames == 0) return 0;
    const auto w = ring.writeFrame.load(std::memory_order_relaxed);
    const auto r = ring.readFrame.load(std::memory_order_acquire);
    const std::size_t used = static_cast<std::size_t>(w - r);
    const std::size_t free = used >= kCapacityFrames ? 0 : kCapacityFrames - used;
    const std::size_t n = frames < free ? frames : free;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t dst = static_cast<std::size_t>((w + i) % kCapacityFrames) * kInputChannels;
        const std::size_t src = i * kInputChannels;
        for (std::size_t ch = 0; ch < kInputChannels; ++ch)
            ring.samples[dst + ch] = interleaved[src + ch];
    }
    ring.writeFrame.store(w + n, std::memory_order_release);
    if (n < frames)
        ring.droppedFrames.fetch_add(frames - n, std::memory_order_relaxed);
    return n;
}

inline std::size_t read(SharedCaptureRing& ring,
                        float* interleaved,
                        std::size_t frames) {
    if (!interleaved || frames == 0) return 0;
    const auto r = ring.readFrame.load(std::memory_order_relaxed);
    const auto w = ring.writeFrame.load(std::memory_order_acquire);
    const std::size_t available = static_cast<std::size_t>(w - r);
    observeQueueDepth(ring, available);
    const std::size_t n = frames < available ? frames : available;
    if (n < frames)
        ring.halUnderrunEvents.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t src = static_cast<std::size_t>((r + i) % kCapacityFrames) * kInputChannels;
        const std::size_t dst = i * kInputChannels;
        for (std::size_t ch = 0; ch < kInputChannels; ++ch)
            interleaved[dst + ch] = ring.samples[src + ch];
    }
    ring.readFrame.store(r + n, std::memory_order_release);
    return n;
}

} // namespace macfw::fw1814::hal::capture
