#pragma once

#include "macfw/am824.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw_hal_capture_shm.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
        // Do not unlink the named object here. coreaudiod is a long-lived
        // consumer and may already have this mapping open. Recreating the name
        // would silently split producer and consumer onto different objects.
    }

    bool open(std::uint32_t sampleRate) {
        bool created = false;
        fd_ = shm_open(macfw::hal::capture::kShmName,
                       O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd_ >= 0) {
            created = true;
        } else if (errno == EEXIST) {
            fd_ = shm_open(macfw::hal::capture::kShmName, O_RDWR, 0);
        }
        if (fd_ < 0) {
            std::fprintf(stderr, "capture shm_open failed: %s\n", std::strerror(errno));
            return false;
        }

        // Only the process that actually created the POSIX SHM object changes
        // its mode. A producer opening an object created by coreaudiod/HAL may
        // not own it, and an unconditional fchmod() then fails with EPERM.
        if (created) {
            if (fchmod(fd_, 0666) != 0) {
                std::fprintf(stderr, "capture fchmod failed: %s\n", std::strerror(errno));
                close(fd_); fd_ = -1;
                return false;
            }
            if (ftruncate(fd_, sizeof(macfw::hal::capture::SharedCaptureRing)) != 0) {
                std::fprintf(stderr, "capture ftruncate failed: %s\n", std::strerror(errno));
                close(fd_); fd_ = -1;
                return false;
            }
        } else {
            struct stat st{};
            if (fstat(fd_, &st) != 0) {
                std::fprintf(stderr, "capture fstat failed: %s\n", std::strerror(errno));
                close(fd_); fd_ = -1;
                return false;
            }
            const std::size_t required = sizeof(macfw::hal::capture::SharedCaptureRing);
            // Darwin may report a POSIX SHM object's backing size rounded up to
            // the VM page size. A larger object is safe because we only mmap
            // the current ABI struct length; reject only a truncated object.
            if (st.st_size < 0 || static_cast<std::size_t>(st.st_size) < required) {
                std::fprintf(stderr,
                             "capture shared ring too small: got %lld, need at least %zu\n",
                             static_cast<long long>(st.st_size), required);
                close(fd_); fd_ = -1;
                return false;
            }
        }

        void* p = mmap(nullptr, sizeof(macfw::hal::capture::SharedCaptureRing),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) {
            std::fprintf(stderr, "capture mmap failed: %s\n", std::strerror(errno));
            close(fd_); fd_ = -1;
            return false;
        }
        ring_ = static_cast<macfw::hal::capture::SharedCaptureRing*>(p);

        // Reinitialize the persistent object in place. Existing HAL mappings
        // remain attached to this exact object; only its producer state resets.
        // Keep the ring inactive until CoreAudio is actually issuing ReadInput
        // and a modest prefill has accumulated. This prevents the HAL consumer
        // from outrunning the bursty NuDCL producer during startup.
        macfw::hal::capture::initialize(*ring_, sampleRate);
        ring_->active.store(0, std::memory_order_release);
        return true;
    }

    bool activateForConsumer(std::size_t prefillFrames) {
        if (!ring_) return false;
        if (ring_->active.load(std::memory_order_acquire) != 0) return true;
        if (ring_->halReadCalls.load(std::memory_order_acquire) == 0) return false;

        const auto w = ring_->writeFrame.load(std::memory_order_acquire);
        const auto r = ring_->readFrame.load(std::memory_order_acquire);
        const std::size_t available = static_cast<std::size_t>(w - r);
        if (available < prefillFrames) return false;

        // If the producer was running before the CoreAudio consumer appeared,
        // discard old backlog and begin close to the live edge with exactly the
        // requested cushion. ReadInput does not advance readFrame while active
        // is zero, so publish the new cursor before publishing active=1.
        ring_->readFrame.store(w - prefillFrames, std::memory_order_release);
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
        if (rx.packetCount() == 0 || rx.packetCount() > lastSignature_.size()) return 0;

        std::size_t totalFrames = 0;
        for (std::size_t checked = 0; checked < rx.packetCount(); ++checked) {
            const std::size_t index = cursor_;
            cursor_ = (cursor_ + 1) % rx.packetCount();

            const auto& slot = rx.slot(index);
            const std::uint64_t signature =
                (static_cast<std::uint64_t>(slot.timestamp) << 32) |
                static_cast<std::uint64_t>(slot.isoHeader);
            if (!slot.touched() || signature == 0 || signature == lastSignature_[index])
                continue;

            lastSignature_[index] = signature;
            totalFrames += decodePacket(slot.packet(), out);
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
    std::array<std::uint64_t, 256> lastSignature_{};
};

} // namespace macfw::transport
