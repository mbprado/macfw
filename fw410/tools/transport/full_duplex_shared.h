#pragma once

#include "macfw/pcm_ring_buffer.h"
#include "macfw_hal_shm.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace macfw::transport::duplex {

constexpr std::size_t kPcmChannels = 10;
constexpr std::size_t kPcmCapacityFrames = 16384;
constexpr std::size_t kCapturePrefillFrames = 4096;
constexpr std::size_t kPlaybackSlots = 640;
constexpr std::size_t kHalfPackets = 320;
constexpr std::size_t kCaptureSlots = 256;
constexpr UInt32 kCaptureMaxPacket = 168;
constexpr UInt32 kPlaybackMaxPacket = 360;
constexpr UInt32 kCyclesPerSecond = 8000;

// CoreAudio physical order -> zero-based FW410 audio position within the
// 10-channel PCM portion of the 11-slot AMDTP stream.
// CoreAudio: A1,A2,A3,A4,A5,A6,A7,A8,SPDIF-L,SPDIF-R
// FW410:     S1,A1,A3,A5,A7,S2,A2,A4,A6,A8
constexpr std::array<std::size_t, macfw::hal::kChannels> kCoreAudioToFw410{
    1, 6, 2, 7, 3, 8, 4, 9, 0, 5
};

inline UInt32 cycleCount(UInt32 cycleTime) {
    return (cycleTime >> 12) & 0x1fffu;
}

class SharedPlaybackReader {
public:
    ~SharedPlaybackReader() {
        if (ring_) munmap(ring_, sizeof(*ring_));
        if (fd_ >= 0) close(fd_);
    }

    bool open() {
        fd_ = shm_open(macfw::hal::kShmName, O_RDWR, 0);
        if (fd_ < 0) return false;
        void* p = mmap(nullptr, sizeof(macfw::hal::SharedPcmRing),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        ring_ = static_cast<macfw::hal::SharedPcmRing*>(p);
        return macfw::hal::valid(*ring_);
    }

    void discardBacklog() {
        if (!ring_) return;
        const auto w = ring_->writeFrame.load(std::memory_order_acquire);
        ring_->readFrame.store(w, std::memory_order_release);
    }

    macfw::hal::SharedPcmRing* ring() { return ring_; }

private:
    int fd_ = -1;
    macfw::hal::SharedPcmRing* ring_ = nullptr;
};

inline std::size_t pumpPlayback(macfw::hal::SharedPcmRing& shared,
                                macfw::PcmRingBuffer& pcm,
                                std::vector<float>& audio,
                                std::vector<std::int32_t>& mapped) {
    const std::size_t frames = std::min<std::size_t>({
        pcm.freeFrames(),
        macfw::hal::availableFrames(shared),
        audio.size() / macfw::hal::kChannels
    });
    if (!frames) return 0;

    const std::size_t got = macfw::hal::read(shared, audio.data(), frames);
    for (std::size_t i = 0; i < got; ++i) {
        const std::size_t base = i * kPcmChannels;
        std::fill_n(mapped.data() + base, kPcmChannels, 0);
        for (std::size_t ch = 0; ch < macfw::hal::kChannels; ++ch) {
            const double s = std::max(
                -1.0,
                std::min(1.0, static_cast<double>(
                    audio[i * macfw::hal::kChannels + ch])));
            mapped[base + kCoreAudioToFw410[ch]] =
                static_cast<std::int32_t>(s * 8388607.0);
        }
    }
    return pcm.write(mapped.data(), got);
}

inline void drainPlayback(macfw::hal::SharedPcmRing& shared,
                          macfw::PcmRingBuffer& pcm,
                          std::vector<float>& audio,
                          std::vector<std::int32_t>& mapped) {
    while (macfw::hal::availableFrames(shared) != 0 &&
           pcm.freeFrames() != 0) {
        if (pumpPlayback(shared, pcm, audio, mapped) == 0) break;
    }
}

} // namespace macfw::transport::duplex
