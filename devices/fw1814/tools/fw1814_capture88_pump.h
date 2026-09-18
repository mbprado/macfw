#pragma once

#include "../channel_map.h"
#include "../hal/include/macfw_fw1814_capture_shm.h"
#include "fw1814_receive_cycle.h"
#include "macfw/am824.h"
#include "macfw/amdtp_receive_ring.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace macfw::fw1814::experimental {

// Experimental 88.2-kHz decoder; captures 16-event blocking packets.
// Keep separate from the released 48-kHz capture path until hardware checks.
class CapturePump88200 {
public:
    struct Stats {
        std::uint64_t dbcDiscontinuities = 0;
        std::uint64_t timestampRegressions = 0;
        std::uint32_t firstRegressionPreviousTimestamp = 0;
        std::uint32_t firstRegressionCurrentTimestamp = 0;
        std::uint64_t reorderedPackets = 0;
        std::uint64_t stalePackets = 0;
        std::uint64_t completedChunks = 0;
        std::uint64_t noDataPackets = 0;
        std::uint64_t metadataByteSwaps = 0;
        std::uint64_t duplicateSlots = 0;
        std::uint64_t incompleteGroups = 0;
        std::uint64_t recoveredGroups = 0;
        std::uint64_t overwrittenGroups = 0;
        std::size_t firstIncompleteSlot = 0;
    };

    std::size_t service(const macfw::AmdtpReceiveRing& rx,
                        macfw::fw1814::hal::capture::SharedCaptureRing& out) {
        constexpr std::size_t kChunkSlots = 32;
        if (rx.packetCount() == 0 || rx.packetCount() > 256)
            return 0;

        std::size_t totalFrames = 0;
        const std::size_t chunkCount =
            (rx.packetCount() + kChunkSlots - 1) / kChunkSlots;
        struct ReadyChunk {
            std::size_t index = 0;
            std::uint32_t timestamp = 0;
        };
        std::array<ReadyChunk, 8> ready{};
        std::size_t readyCount = 0;
        for (std::size_t chunk = 0; chunk < chunkCount; ++chunk) {
            const std::size_t begin = chunk * kChunkSlots;
            const std::size_t end = std::min(rx.packetCount(), begin + kChunkSlots);
            const auto& terminal = rx.slot(end - 1);
            if (!terminal.touched() || terminal.timestamp == 0 ||
                terminal.timestamp == lastChunkTimestamp_[chunk])
                continue;

            if (terminal.timestamp != observedChunkTimestamp_[chunk]) {
                if (incompletePending_[chunk] &&
                    lastChunkTimestamp_[chunk] != observedChunkTimestamp_[chunk])
                    ++stats_.overwrittenGroups;
                observedChunkTimestamp_[chunk] = terminal.timestamp;
                incompletePending_[chunk] = false;
            }

            // The terminal header can change before the group is published.
            // Waiting for every slot's timestamp to advance prevents a
            // partial group from being decoded as a new 32-cycle capture.
            bool complete = true;
            for (std::size_t slotIndex = begin; slotIndex < end; ++slotIndex) {
                const auto& slot = rx.slot(slotIndex);
                if (slot.timestamp == 0 ||
                    slot.timestamp == lastSlotTimestamp_[slotIndex]) {
                    complete = false;
                    if (!incompletePending_[chunk]) {
                        ++stats_.incompleteGroups;
                        stats_.firstIncompleteSlot = slotIndex;
                        incompletePending_[chunk] = true;
                    }
                    break;
                }
            }
            if (!complete) continue;

            if (incompletePending_[chunk]) {
                ++stats_.recoveredGroups;
                incompletePending_[chunk] = false;
            }

            ready[readyCount++] = {chunk, terminal.timestamp};
        }

        // The ring's index zero is not a time origin. In particular, when
        // publication spans the last and first chunks, processing index zero
        // first can make valid earlier packets look stale by DBC. All ready
        // chunks fit within one RX rotation (32 ms). Compare the cycle field
        // so a seconds-field rollover does not reverse the ordering.
        std::sort(ready.begin(), ready.begin() + readyCount,
                  [](const ReadyChunk& a, const ReadyChunk& b) {
                      return receiveTimestampAfter(b.timestamp, a.timestamp);
                  });
        for (std::size_t i = 0; i < readyCount; ++i) {
            const auto chunk = ready[i].index;
            lastChunkTimestamp_[chunk] = ready[i].timestamp;
            ++stats_.completedChunks;
            const std::size_t begin = chunk * kChunkSlots;
            totalFrames += processChunk(
                rx, begin, std::min(rx.packetCount(), begin + kChunkSlots), out);
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
        std::size_t events = 0;
        std::size_t originalOrder = 0;
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
            // A changed terminal DCL is not sufficient proof that every slot
            // in this chunk contains a new cycle. When a publication races
            // with inspection, the same completed slot can be seen twice.
            // A given slot is reused only after a full receive-ring rotation,
            // so its normalized cycle timestamp must change for new data.
            if (slot.timestamp != 0 &&
                slot.timestamp == lastSlotTimestamp_[index]) {
                ++stats_.duplicateSlots;
                continue;
            }
            if (slot.timestamp != 0)
                lastSlotTimestamp_[index] = slot.timestamp;
            if (slot.metadataByteSwapped) ++stats_.metadataByteSwaps;
            if (slot.packetLength() > slot.capacity) {
                out.malformedPackets.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const auto packet = slot.packet();
            if (!packet.hasCip()) continue;
            const auto h = packet.cip();

            // FW1814 NODATA packets are eight-byte CIP-only packets and were
            // observed with DBS=2. Classify them by NODATA semantics before
            // applying the DBS=11 data-packet validator.
            if (packet.length == 8) {
                if (h.syt == 0xffffu) ++stats_.noDataPackets;
                continue;
            }

            // Capture formation is 10 PCM + 1 MIDI = 11 quadlets per AM824
            // event. kCapturePcmPositions is intentionally only the PCM count;
            // the complete event/DBS width is kCaptureStreamPositions.
            constexpr std::size_t kEventBytes = kCaptureStreamPositions * 4;
            if (h.dbs != kCaptureStreamPositions || h.fmt != 0x10 ||
                h.fdf != 0x03 || packet.dataLength() == 0 ||
                packet.dataLength() % kEventBytes != 0) {
                out.malformedPackets.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const std::size_t events = packet.dataLength() / kEventBytes;
            if (events == 0 || events > 16 || count >= candidates.size()) {
                out.malformedPackets.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            candidates[count] = Candidate{packet, slot.timestamp, events, count};
            ++count;
        }

        if (count == 0) return 0;
        std::size_t frames = 0;
        // A full 32-cycle group can contain 22 data packets. DBC advances
        // by 16 per packet and repeats after 16 packets, so it cannot order
        // all candidates in this group. Receive timestamps identify the
        // actual cycle; check DBC only after arranging packets in time.
        std::sort(candidates.begin(), candidates.begin() + count,
                  [](const Candidate& a, const Candidate& b) {
                      return receiveTimestampAfter(b.timestamp, a.timestamp);
                  });
        for (std::size_t i = 0; i < count; ++i) {
            const auto& candidate = candidates[i];
            if (haveTimestamp_ &&
                !receiveTimestampAfter(candidate.timestamp, lastTimestamp_)) {
                ++stats_.stalePackets;
                continue;
            }
            if (candidate.originalOrder != i)
                ++stats_.reorderedPackets;
            frames += decode(candidate, out);
        }
        return frames;
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
            !receiveTimestampAfter(candidate.timestamp, lastTimestamp_)) {
            if (stats_.timestampRegressions == 0) {
                stats_.firstRegressionPreviousTimestamp = lastTimestamp_;
                stats_.firstRegressionCurrentTimestamp = candidate.timestamp;
            }
            ++stats_.timestampRegressions;
        }
        lastTimestamp_ = candidate.timestamp;
        haveTimestamp_ = true;

        constexpr std::size_t kMaxEvents = 16;
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
            p += kCaptureStreamPositions * 4;
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

    std::array<std::uint32_t, 8> lastChunkTimestamp_{};
    std::array<std::uint32_t, 8> observedChunkTimestamp_{};
    std::array<bool, 8> incompletePending_{};
    std::array<std::uint32_t, 256> lastSlotTimestamp_{};
    Stats stats_{};
    std::array<float, macfw::fw1814::hal::capture::kInputChannels> meterPeaks_{};
    bool haveExpectedDbc_ = false;
    std::uint8_t expectedDbc_ = 0;
    bool haveTimestamp_ = false;
    std::uint32_t lastTimestamp_ = 0;
};

} // namespace macfw::fw1814::experimental
