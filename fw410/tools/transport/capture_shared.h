#pragma once

#include "macfw/am824.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw_hal_capture_shm.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
        std::uint64_t completedChunks = 0;
    };

    explicit CaptureReceivePump(std::uint8_t expectedFdf,
                                bool timestampOnlyChunkToken = false)
        : expectedFdf_(expectedFdf),
          timestampOnlyChunkToken_(timestampOnlyChunkToken) {}

    std::size_t service(const macfw::AmdtpReceiveRing& rx,
                        macfw::hal::capture::SharedCaptureRing& out) {
        constexpr std::size_t kChunkSlots = 32;
        if (rx.packetCount() == 0 || rx.packetCount() > 256) return 0;

        std::size_t totalFrames = 0;
        const std::size_t chunkCount = (rx.packetCount() + kChunkSlots - 1) / kChunkSlots;

        for (std::size_t chunk = 0; chunk < chunkCount; ++chunk) {
            const std::size_t begin = chunk * kChunkSlots;
            const std::size_t end = (begin + kChunkSlots < rx.packetCount())
                ? begin + kChunkSlots : rx.packetCount();
            const std::size_t terminalIndex = end - 1;

            const auto& terminal = rx.slot(terminalIndex);
            const std::uint64_t terminalSignature = timestampOnlyChunkToken_
                ? static_cast<std::uint64_t>(terminal.timestamp)
                : ((static_cast<std::uint64_t>(terminal.timestamp) << 32) |
                   static_cast<std::uint64_t>(terminal.isoHeader));
            if (!terminal.touched() || terminal.timestamp == 0 ||
                terminalSignature == lastChunkSignature_[chunk])
                continue;

            lastChunkSignature_[chunk] = terminalSignature;
            ++stats_.completedChunks;
            totalFrames += processCompletedChunk(rx, begin, end, out);
        }

        return totalFrames;
    }

    const Stats& stats() const { return stats_; }
    const std::array<float, macfw::hal::capture::kChannels>& meterPeaks() const {
        return meterPeaks_;
    }

private:
    struct Candidate {
        macfw::amdtp::PacketView packet{};
        std::uint32_t timestamp = 0;
        std::uint8_t dbc = 0;
        std::size_t events = 0;
        std::size_t index = 0;
        bool used = false;
    };

    std::size_t processCompletedChunk(const macfw::AmdtpReceiveRing& rx,
                                      std::size_t begin,
                                      std::size_t end,
                                      macfw::hal::capture::SharedCaptureRing& out) {
        std::array<Candidate, 32> candidates{};
        std::size_t candidateCount = 0;

        for (std::size_t index = begin; index < end; ++index) {
            const auto& slot = rx.slot(index);
            if (!slot.touched()) continue;

            const auto packet = slot.packet();
            if (!packet.hasCip() || packet.length <= 8) continue;
            const auto h = packet.cip();
            if (packet.dataLength() % 20 != 0) {
                out.malformedPackets.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const std::size_t events = packet.dataLength() / 20;
            if (h.dbs != 5 || h.fmt != 0x10 || h.fdf != expectedFdf_ ||
                events == 0 || events > 8) {
                out.malformedPackets.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            candidates[candidateCount++] = Candidate{packet, slot.timestamp, h.dbc, events, index, false};
        }

        if (candidateCount == 0) return 0;
        std::size_t totalFrames = 0;
        std::size_t emitted = 0;

        if (!haveExpectedDbc_) {
            std::size_t first = oldestUnused(candidates, candidateCount);
            totalFrames += decodeCandidate(candidates[first], out);
            candidates[first].used = true;
            ++emitted;
        }

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

        while (emitted < candidateCount) {
            ++stats_.dbcDiscontinuities;
            const std::size_t next = oldestUnused(candidates, candidateCount);
            if (next == candidateCount) break;
            const std::uint8_t delta = static_cast<std::uint8_t>(candidates[next].dbc - expectedDbc_);
            if (delta > 128) {
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

    template <std::size_t N>
    static std::size_t firstUnused(const std::array<Candidate, N>& candidates,
                                   std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
            if (!candidates[i].used) return i;
        return count;
    }

    template <std::size_t N>
    static std::size_t oldestUnused(const std::array<Candidate, N>& candidates,
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
        constexpr float kMeterDecay = 0.92f;
        std::array<float, kMaxEvents * macfw::hal::capture::kChannels> decoded{};
        std::array<float, macfw::hal::capture::kChannels> peaks{};
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
            for (std::size_t ch = 0; ch < peaks.size(); ++ch)
                peaks[ch] = std::max(peaks[ch], std::fabs(decoded[b + ch]));
            p += 20;
        }
        for (std::size_t ch = 0; ch < meterPeaks_.size(); ++ch)
            meterPeaks_[ch] = std::max(peaks[ch], meterPeaks_[ch] * kMeterDecay);

        if (invalid) out.invalidLabels.fetch_add(invalid, std::memory_order_relaxed);
        out.decodedPackets.fetch_add(1, std::memory_order_relaxed);
        out.decodedFrames.fetch_add(candidate.events, std::memory_order_relaxed);
        return macfw::hal::capture::write(out, decoded.data(), candidate.events);
    }

    std::uint8_t expectedFdf_ = 0;
    bool timestampOnlyChunkToken_ = false;
    std::array<std::uint64_t, 8> lastChunkSignature_{};
    Stats stats_{};
    std::array<float, macfw::hal::capture::kChannels> meterPeaks_{};
    bool haveExpectedDbc_ = false;
    std::uint8_t expectedDbc_ = 0;
    bool haveTimestamp_ = false;
    std::uint32_t lastTimestamp_ = 0;
};

} // namespace macfw::transport
