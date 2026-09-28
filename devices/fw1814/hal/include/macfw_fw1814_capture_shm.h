#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace macfw::fw1814::hal::capture {

constexpr const char* kShmName = "/macfw_fw1814_capture_v2";
constexpr std::uint32_t kMagic = 0x4d313849u; // M18I
constexpr std::uint32_t kVersion = 2;
constexpr std::uint32_t kInputChannels = 8;
constexpr std::uint32_t kCapacityFrames = 32768;
constexpr std::uint64_t kQueueMinUnset = std::numeric_limits<std::uint64_t>::max();

enum class DiscardReason : std::uint32_t {
    none = 0,
    clientStart = 1,
    liveTrim = 2,
    staleFlush = 3,
};

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
    // Appended so existing ring offsets stay unchanged. The version/size check
    // prevents a new HAL from accessing these fields in an old mapping.
    std::atomic<std::uint64_t> halDiscardEvents;
    std::atomic<std::uint64_t> halDiscardedFrames;
    std::atomic<std::uint64_t> halLastDiscardTick;
    std::atomic<std::uint64_t> halLastDiscardQueued;
    std::atomic<std::uint32_t> halLastDiscardReason;
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
    ring.halDiscardEvents.store(0, std::memory_order_relaxed);
    ring.halDiscardedFrames.store(0, std::memory_order_relaxed);
    ring.halLastDiscardTick.store(0, std::memory_order_relaxed);
    ring.halLastDiscardQueued.store(0, std::memory_order_relaxed);
    ring.halLastDiscardReason.store(0, std::memory_order_relaxed);
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

inline void recordDiscard(SharedCaptureRing& ring, std::size_t frames,
                          std::size_t queued, DiscardReason reason,
                          std::uint64_t tick) {
    if (frames == 0) return;
    ring.droppedFrames.fetch_add(frames, std::memory_order_relaxed);
    ring.halDiscardedFrames.fetch_add(frames, std::memory_order_relaxed);
    ring.halLastDiscardTick.store(tick, std::memory_order_relaxed);
    ring.halLastDiscardQueued.store(queued, std::memory_order_relaxed);
    ring.halLastDiscardReason.store(static_cast<std::uint32_t>(reason),
                                    std::memory_order_relaxed);
    ring.halDiscardEvents.fetch_add(1, std::memory_order_release);
}

// A new 48 kHz client must not inherit samples left by the previous one.
// The producer will activate the ring again after a fresh 512-frame prefill.
inline std::size_t discardForClientStart(SharedCaptureRing& ring,
                                         std::uint64_t tick) {
    ring.active.store(0, std::memory_order_release);
    const auto w = ring.writeFrame.load(std::memory_order_acquire);
    const auto r = ring.readFrame.load(std::memory_order_acquire);
    const auto queued = static_cast<std::size_t>(w - r);
    ring.readFrame.store(w, std::memory_order_release);
    recordDiscard(ring, queued, queued, DiscardReason::clientStart, tick);
    return queued;
}

// Preserve enough fresh capture for multiple HAL callbacks while preventing
// moderate scheduling stalls from becoming persistent input latency.
inline std::size_t trimLiveCapture(SharedCaptureRing& ring,
                                   std::size_t maxQueuedFrames,
                                   std::size_t targetFrames,
                                   std::uint64_t tick) {
    const auto w = ring.writeFrame.load(std::memory_order_acquire);
    const auto r = ring.readFrame.load(std::memory_order_acquire);
    const auto queued = static_cast<std::size_t>(w - r);
    if (queued <= maxQueuedFrames || targetFrames >= queued) return 0;
    const auto discarded = queued - targetFrames;
    ring.readFrame.store(w - targetFrames, std::memory_order_release);
    recordDiscard(ring, discarded, queued, DiscardReason::liveTrim, tick);
    return discarded;
}

// Called by the HAL reader at every rate. When no client reads for long
// enough, the producer fills the ring and drops all subsequent (new) frames.
// Keeping even a small part of that full ring would replay old audio before
// fresh samples. Suspend reads, discard the whole backlog, and let the
// transport prefill again from current capture packets before reactivation.
inline bool suspendStaleCapture(SharedCaptureRing& ring,
                                std::size_t maxQueuedFrames,
                                std::uint64_t tick = 0) {
    const auto w = ring.writeFrame.load(std::memory_order_acquire);
    const auto r = ring.readFrame.load(std::memory_order_acquire);
    const auto queued = static_cast<std::size_t>(w - r);
    if (queued <= maxQueuedFrames) return false;
    ring.active.store(0, std::memory_order_release);
    ring.readFrame.store(w, std::memory_order_release);
    recordDiscard(ring, queued, queued, DiscardReason::staleFlush, tick);
    return true;
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

// The shared-memory ABI remains eight-channel so released low/high-rate
// engines can coexist. Quad-rate hardware exposes only its first two PCM
// positions; compact those physical inputs for CoreAudio without changing
// the persistent ring layout.
inline std::size_t readFirstChannels(SharedCaptureRing& ring,
                                     float* interleaved,
                                     std::size_t frames,
                                     std::size_t channels) {
    if (!interleaved || frames == 0 || channels == 0 ||
        channels > kInputChannels)
        return 0;
    const auto r = ring.readFrame.load(std::memory_order_relaxed);
    const auto w = ring.writeFrame.load(std::memory_order_acquire);
    const std::size_t available = static_cast<std::size_t>(w - r);
    observeQueueDepth(ring, available);
    const std::size_t n = frames < available ? frames : available;
    if (n < frames)
        ring.halUnderrunEvents.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t src =
            static_cast<std::size_t>((r + i) % kCapacityFrames) * kInputChannels;
        const std::size_t dst = i * channels;
        for (std::size_t ch = 0; ch < channels; ++ch)
            interleaved[dst + ch] = ring.samples[src + ch];
    }
    ring.readFrame.store(r + n, std::memory_order_release);
    return n;
}

} // namespace macfw::fw1814::hal::capture
