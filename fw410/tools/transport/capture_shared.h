#pragma once

#include "macfw/am824.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw_hal_capture_shm.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace macfw::transport {

class CaptureSharedWriter {
public:
    ~CaptureSharedWriter() {
        if (ring_) {
            ring_->active.store(0, std::memory_order_release);
            munmap(ring_, sizeof(*ring_));
        }
        if (fd_ >= 0) close(fd_);
    }

    bool open(std::uint32_t sampleRate) {
        fd_ = shm_open(macfw::hal::capture::kShmName, O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) return false;
        if (ftruncate(fd_, sizeof(macfw::hal::capture::SharedCaptureRing)) != 0) return false;
        void* p = mmap(nullptr, sizeof(macfw::hal::capture::SharedCaptureRing),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) return false;
        ring_ = static_cast<macfw::hal::capture::SharedCaptureRing*>(p);
        macfw::hal::capture::initialize(*ring_, sampleRate);
        ring_->active.store(1, std::memory_order_release);
        return true;
    }

    macfw::hal::capture::SharedCaptureRing* ring() { return ring_; }

private:
    int fd_ = -1;
    macfw::hal::capture::SharedCaptureRing* ring_ = nullptr;
};

class CaptureReceivePump {
public:
    explicit CaptureReceivePump(std::uint8_t expectedFdf) : expectedFdf_(expectedFdf) {}

    std::size_t service(const macfw::AmdtpReceiveRing& rx,
                        macfw::hal::capture::SharedCaptureRing& out) {
        if (rx.packetCount() == 0) return 0;
        if (lastSignature_.size() != rx.packetCount()) {
            lastSignature_.fill(0);
            cursor_ = 0;
        }

        std::size_t totalFrames = 0;
        for (std::size_t checked = 0; checked < rx.packetCount(); ++checked) {
            const auto& slot = rx.slot(cursor_);
            const std::uint64_t signature =
                (static_cast<std::uint64_t>(slot.timestamp) << 32) |
                static_cast<std::uint64_t>(slot.isoHeader);
            if (!slot.touched() || signature == 0 || signature == lastSignature_[cursor_])
                break;

            lastSignature_[cursor_] = signature;
            totalFrames += decodePacket(slot.packet(), out);
            cursor_ = (cursor_ + 1) % rx.packetCount();
        }
        return totalFrames;
    }

private:
    std::size_t decodePacket(const macfw::amdtp::PacketView& packet,
                             macfw::hal::capture::SharedCaptureRing& out) {
        if (!packet.hasCip() || packet.length <= 8) return 0;
        const auto h = packet.cip();
        if (h.dbs != 5 || h.fmt != 0x10 || h.fdf != expectedFdf_) {
            out.malformedPackets.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        if (packet.dataLength() % 20 != 0) {
            out.malformedPackets.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }

        constexpr std::size_t kMaxEvents = 8;
        std::array<float, kMaxEvents * macfw::hal::capture::kChannels> decoded{};
        const std::size_t events = packet.dataLength() / 20;
        if (events > kMaxEvents) {
            out.malformedPackets.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }

        const std::uint8_t* p = packet.data();
        std::uint64_t invalid = 0;
        for (std::size_t event = 0; event < events; ++event) {
            std::int32_t raw[4] = {};
            for (std::size_t pos = 0; pos < 4; ++pos) {
                if (!macfw::am824::decodeMbla24(macfw::am824::be32(p + pos * 4), raw[pos])) {
                    raw[pos] = 0;
                    ++invalid;
                }
            }
            // Device stream positions:
            //   0 S/PDIF L, 1 Analog 1, 2 S/PDIF R, 3 Analog 2, 4 MIDI.
            // CoreAudio-facing order:
            //   Analog 1, Analog 2, S/PDIF L, S/PDIF R.
            const std::size_t b = event * macfw::hal::capture::kChannels;
            decoded[b + 0] = static_cast<float>(raw[1] / 8388608.0);
            decoded[b + 1] = static_cast<float>(raw[3] / 8388608.0);
            decoded[b + 2] = static_cast<float>(raw[0] / 8388608.0);
            decoded[b + 3] = static_cast<float>(raw[2] / 8388608.0);
            p += 20;
        }

        if (invalid) out.invalidLabels.fetch_add(invalid, std::memory_order_relaxed);
        out.decodedPackets.fetch_add(1, std::memory_order_relaxed);
        out.decodedFrames.fetch_add(events, std::memory_order_relaxed);
        return macfw::hal::capture::write(out, decoded.data(), events);
    }

    std::uint8_t expectedFdf_ = 0;
    std::size_t cursor_ = 0;
    // Current receive-ring size is 256 slots. Keep this fixed-size and allocation-free.
    std::array<std::uint64_t, 256> lastSignature_{};
};

} // namespace macfw::transport
