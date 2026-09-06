#pragma once

#include "../channel_map.h"
#include "../hal/include/macfw_fw1814_capture_shm.h"
#include "macfw/am824.h"
#include "macfw/amdtp_receive_ring.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace macfw::fw1814::transport {

class CapturePump48k {
public:
    struct Stats {
        std::uint64_t dbcDiscontinuities = 0;
        std::uint64_t timestampRegressions = 0;
        std::uint64_t reorderedPackets = 0;
        std::uint64_t stalePackets = 0;
        std::uint64_t completedChunks = 0;
        std::uint64_t noDataPackets = 0;
    };

    std::size_t service(const macfw::AmdtpReceiveRing& rx,
                        macfw::fw1814::hal::capture::SharedCaptureRing& out) {
        constexpr std::size_t kChunkSlots = 32;
        if (rx.packetCount() == 0 || rx.packetCount() > 256)
            return 0;

        std::size_t totalFrames = 0;
        const std::size_t chunkCount =
            (rx.packetCount() + kChunkSlots - 1) / kChunkSlots;
        for (std::size_t chunk = 0; chunk < chunkCount; ++chunk) {
            const std::size_t begin = chunk * kChunkSlots;
            const std::size_t end = std::min(rx.packetCount(), begin + kChunkSlots);
            const auto& terminal = rx.slot(end - 1);
            const std::uint64_t signature =
                (static_cast<std::uint64_t>(terminal.timestamp) << 32) |
                static_cast<std::uint64_t>(terminal.isoHeader);
            if (!terminal.touched() || terminal.timestamp == 0 ||
                signature == lastChunkSignature_[chunk])
                continue;

            lastChunkSignature_[chunk] = signature;
            ++stats_.completedChunks;
            totalFrames += processChunk(rx, begin, end, out);
        }
        return totalFrames;
    }

    const Stats& stats() const { return stats_; }
    const std::array<float, macfw::fw1814::hal::capture::kInputChannels>&
    meterPeaks() const { return meterPeaks_; }

private:
    struct Candidate {
        macfw::amdtp::PacketView packet{};
        std::uint32_t timestamp = 0;
        std::uint8_t dbc = 0;
        std::size_t events = 0;
        bool used = false;
    };

    std::size_t processChunk(
        const macfw::AmdtpReceiveRing& rx,
        std::size_t begin,
        std::size_t end,
        macfw::fw1814::hal::capture::SharedCaptureRing& out) {
        std::array<Candidate, 32> candidates{};
        std::size_t count = 0;

        for (std::size_t index = begin; index < end; ++index) {
            const auto& slot = rx.slot(index);
            if (!slot.touched()) continue;
            const auto packet = slot.packet();
            if (!packet.hasCip()) continue;
            const auto h = packet.cip();

            if (packet.length == 8) {
                if (h.syt == 0xffffu) ++stats_.noDataPackets;
                continue;
            }

            constexpr std::size_t kEventBytes = kCapturePcmPositions * 4;
            if (h.dbs != kCapturePcmPositions || h.fmt != 0x10 ||
                h.fdf != 0x02 || packet.dataLength() == 0 ||
                packet.dataLength() % kEventBytes != 0) {
                out.malformedPackets.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const std::size_t events = packet.dataLength() / kEventBytes;
            if (events == 0 || events > 8 || count >= candidates.size()) {
                out.malformedPackets.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            candidates[count++] = Candidate{packet, slot.timestamp, h.dbc, events, false};
        }

        if (count == 0) return 0;
        std::size_t frames = 0;
        std::size_t emitted = 0;

        if (!haveExpectedDbc_) {
            const std::size_t first = oldestUnused(candidates, count);
            frames += decode(candidates[first], out);
            candidates[first].used = true;
            ++emitted;
        }

        while (emitted < count) {
            const std::size_t match = findExpected(candidates, count);
            if (match == count) break;
            if (match != firstUnused(candidates, count))
                ++stats_.reorderedPackets;
            frames += decode(candidates[match], out);
            candidates[match].used = true;
            ++emitted;
        }

        while (emitted < count) {
            ++stats_.dbcDiscontinuities;
            const std::size_t next = oldestUnused(candidates, count);
            if (next == count) break;
            const std::uint8_t delta =
                static_cast<std::uint8_t>(candidates[next].dbc - expectedDbc_);
            if (delta > 128) {
                candidates[next].used = true;
                ++emitted;
                ++stats_.stalePackets;
                continue;
            }

            frames += decode(candidates[next], out);
            candidates[next].used = true;
            ++emitted;

            while (emitted < count) {
                const std::size_t match = findExpected(candidates, count);
                if (match == count) break;
                if (match != firstUnused(candidates, count))
                    ++stats_.reorderedPackets;
                frames += decode(candidates[match], out);
                candidates[match].used = true;
                ++emitted;
            }
        }
        return frames;
    }

    template <std::size_t N>
    std::size_t findExpected(const std::array<Candidate, N>& c,
                             std::size_t count) const {
        for (std::size_t i = 0; i < count; ++i)
            if (!c[i].used && c[i].dbc == expectedDbc_) return i;
        return count;
    }

    template <std::size_t N>
    static std::size_t firstUnused(const std::array<Candidate, N>& c,
                                   std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
            if (!c[i].used) return i;
        return count;
    }

    template <std::size_t N>
    static std::size_t oldestUnused(const std::array<Candidate, N>& c,
                                    std::size_t count) {
        std::size_t oldest = count;
        for (std::size_t i = 0; i < count; ++i) {
            if (c[i].used) continue;
            if (oldest == count ||
                static_cast<std::int32_t>(c[i].timestamp - c[oldest].timestamp) < 0)
                oldest = i;
        }
        return oldest;
    }

    std::size_t decode(
        const Candidate& candidate,
        macfw::fw1814::hal::capture::SharedCaptureRing& out) {
        const auto h = candidate.packet.cip();
        if (haveExpectedDbc_ && h.dbc != expectedDbc_)
            ++stats_.dbcDiscontinuities;
        expectedDbc_ = static_cast<std::uint8_t>(h.dbc + candidate.events);
        haveExpectedDbc_ = true;

        if (haveTimestamp_ &&
            static_cast<std::int32_t>(candidate.timestamp - lastTimestamp_) <= 0)
            ++stats_.timestampRegressions;
        lastTimestamp_ = candidate.timestamp;
        haveTimestamp_ = true;

        constexpr std::size_t kMaxEvents = 8;
        constexpr float kMeterDecay = 0.92f;
        std::array<float, kMaxEvents * macfw::fw1814::hal::capture::kInputChannels>
            decoded{};
        std::array<float, macfw::fw1814::hal::capture::kInputChannels> peaks{};
        std::uint64_t invalid = 0;
        const std::uint8_t* p = candidate.packet.data();

        for (std::size_t event = 0; event < candidate.events; ++event) {
            const std::size_t base =
                event * macfw::fw1814::hal::capture::kInputChannels;
            for (std::size_t physical = 0;
                 physical < macfw::fw1814::hal::capture::kInputChannels;
                 ++physical) {
                const std::size_t pos = kCapturePositionForAnalogInput[physical];
                std::int32_t raw = 0;
                if (!macfw::am824::decodeMbla24(
                        macfw::am824::be32(p + pos * 4), raw)) {
                    raw = 0;
                    ++invalid;
                }
                const float value = static_cast<float>(raw / 8388608.0);
                decoded[base + physical] = value;
                peaks[physical] = std::max(peaks[physical], std::fabs(value));
            }
            p += kCapturePcmPositions * 4;
        }

        for (std::size_t ch = 0; ch < meterPeaks_.size(); ++ch)
            meterPeaks_[ch] = std::max(peaks[ch], meterPeaks_[ch] * kMeterDecay);

        if (invalid)
            out.invalidLabels.fetch_add(invalid, std::memory_order_relaxed);
        out.decodedPackets.fetch_add(1, std::memory_order_relaxed);
        out.decodedFrames.fetch_add(candidate.events, std::memory_order_relaxed);
        return macfw::fw1814::hal::capture::write(
            out, decoded.data(), candidate.events);
    }

    std::array<std::uint64_t, 8> lastChunkSignature_{};
    Stats stats_{};
    std::array<float, macfw::fw1814::hal::capture::kInputChannels> meterPeaks_{};
    bool haveExpectedDbc_ = false;
    std::uint8_t expectedDbc_ = 0;
    bool haveTimestamp_ = false;
    std::uint32_t lastTimestamp_ = 0;
};

} // namespace macfw::fw1814::transport
