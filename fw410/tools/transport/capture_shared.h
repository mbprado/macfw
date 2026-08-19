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
    struct Stats {
        std::uint64_t dbcDiscontinuities = 0;
        std::uint64_t timestampRegressions = 0;
        std::uint64_t reorderedPackets = 0;
        std::uint64_t stalePackets = 0;
    };

    explicit CaptureReceivePump(std::uint8_t expectedFdf) : expectedFdf_(expectedFdf) {}

    std::size_t service(const macfw::AmdtpReceiveRing& rx,
                        macfw::hal::capture::SharedCaptureRing& out) {
        if (rx.packetCount() == 0 || rx.packetCount() > lastSignature_.size()) return 0;

        std::array<Candidate, 256> candidates{};
        std::size_t candidateCount = 0;

        // First snapshot all newly published receive slots. Do not decode while
        // walking the DCL array: a NuDCL update chunk can become visible with a
        // ring-wrap boundary in the middle, so array-index order is not always
        // bus-time order.
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
            const auto packet = slot.packet();
            if (!packet.hasCip() || packet.length <= 8) continue;
            const auto h = packet.cip();
            const std::size_t events = packet.dataLength() / 20;
            if (h.dbs != 5 || h.fmt != 0x10 || h.fdf != expectedFdf_ ||
                packet.dataLength() % 20 != 0 || events == 0 || events > 8) {
                out.malformedPackets.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            candidates[candidateCount++] = Candidate{packet, slot.timestamp, h.dbc, events, index, false};
        }

        if (candidateCount == 0) return 0;

        std::size_t totalFrames = 0;
        std::size_t emitted = 0;

        // At startup we do not yet have a DBC expectation. Pick the oldest
        // timestamp in this snapshot as the sequence anchor; after that DBC is
        // authoritative and naturally wraps at 8 bits.
        if (!haveExpectedDbc_) {
            std::size_t first = 0;
            for (std::size_t i = 1; i < candidateCount; ++i) {
                const auto delta = static_cast<std::int32_t>(candidates[i].timestamp -
                                                             candidates[first].timestamp);
                if (delta < 0) first = i;
            }
            totalFrames += decodeCandidate(candidates[first], out);
            candidates[first].used = true;
            ++emitted;
        }

        // Greedily emit exactly the packet that continues the current DBC. This
        // reorders chunks that became visible out of DCL-array order without
        // inventing samples or changing the HAL buffering model.
        while (emitted < candidateCount) {
            std::size_t match = candidateCount;
            for (std::size_t i = 0; i < candidateCount; ++i) {
                if (!candidates[i].used && candidates[i].dbc == expectedDbc_) {
                    match = i;
                    break;
                }
            }
            if (match == candidateCount) break;
            if (match != firstUnused(candidates, candidateCount)) ++stats_.reorderedPackets;
            totalFrames += decodeCandidate(candidates[match], out);
            candidates[match].used = true;
            ++emitted;
        }

        // Anything left cannot continue the current DBC sequence. Keep the
        // discontinuity visible, then resync to the oldest remaining packet so
        // capture continues instead of stalling indefinitely.
        while (emitted < candidateCount) {
            ++stats_.dbcDiscontinuities;
            std::size_t next = oldestUnused(candidates, candidateCount);
            if (next == candidateCount) break;
            const std::uint8_t delta = static_cast<std::uint8_t>(candidates[next].dbc - expectedDbc_);
            if (delta > 128) {
                // Packet is behind the live sequence (duplicate/stale snapshot).
                candidates[next].used = true;
                ++emitted;
                ++stats_.stalePackets;
                continue;
            }
            totalFrames += decodeCandidate(candidates[next], out);
            candidates[next].used = true;
            ++emitted;

            while (emitted < candidateCount) {
                std::size_t match = candidateCount;
                for (std::size_t i = 0; i < candidateCount; ++i) {
                    if (!candidates[i].used && candidates[i].dbc == expectedDbc_) {
                        match = i;
                        break;
                    }
                }
                if (match == candidateCount) break;
                if (match != firstUnused(candidates, candidateCount)) ++stats_.reorderedPackets;
                totalFrames += decodeCandidate(candidates[match], out);
                candidates[match].used = true;
                ++emitted;
            }
        }

        return totalFrames;
    }

    const Stats& stats() const { return stats_; }

private:
    struct Candidate {
        macfw::amdtp::PacketView packet{};
        std::uint32_t timestamp = 0;
        std::uint8_t dbc = 0;
        std::size_t events = 0;
        std::size_t index = 0;
        bool used = false;
    };

    static std::size_t firstUnused(const std::array<Candidate, 256>& candidates,
                                   std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
            if (!candidates[i].used) return i;
        return count;
    }

    static std::size_t oldestUnused(const std::array<Candidate, 256>& candidates,
                                    std::size_t count) {
        std::size_t oldest = count;
        for (std::size_t i = 0; i < count; ++i) {
            if (candidates[i].used) continue;
            if (oldest == count) {
                oldest = i;
                continue;
            }
            const auto delta = static_cast<std::int32_t>(candidates[i].timestamp -
                                                         candidates[oldest].timestamp);
            if (delta < 0) oldest = i;
        }
        return oldest;
    }

    std::size_t decodeCandidate(const Candidate& candidate,
                                macfw::hal::capture::SharedCaptureRing& out) {
        const auto h = candidate.packet.cip();

        if (haveExpectedDbc_ && h.dbc != expectedDbc_)
            ++stats_.dbcDiscontinuities;
        expectedDbc_ = static_cast<std::uint8_t>(h.dbc + candidate.events);
        haveExpectedDbc_ = true;

        if (haveTimestamp_) {
            const auto delta = static_cast<std::int32_t>(candidate.timestamp - lastTimestamp_);
            if (delta <= 0) ++stats_.timestampRegressions;
        }
        lastTimestamp_ = candidate.timestamp;
        haveTimestamp_ = true;

        constexpr std::size_t kMaxEvents = 8;
        std::array<float, kMaxEvents * macfw::hal::capture::kChannels> decoded{};
        const std::uint8_t* p = candidate.packet.data();
        std::uint64_t invalid = 0;
        for (std::size_t event = 0; event < candidate.events; ++event) {
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
        out.decodedFrames.fetch_add(candidate.events, std::memory_order_relaxed);
        return macfw::hal::capture::write(out, decoded.data(), candidate.events);
    }

    std::uint8_t expectedFdf_ = 0;
    std::size_t cursor_ = 0;
    std::array<std::uint64_t, 256> lastSignature_{};
    Stats stats_{};
    bool haveExpectedDbc_ = false;
    std::uint8_t expectedDbc_ = 0;
    bool haveTimestamp_ = false;
    std::uint32_t lastTimestamp_ = 0;
};

} // namespace macfw::transport
