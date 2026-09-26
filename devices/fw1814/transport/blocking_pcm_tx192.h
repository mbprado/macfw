#pragma once

#include "macfw/am824_playback.h"
#include "macfw/firewire_device.h"
#include "macfw/pcm_ring_buffer.h"

#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <utility>

namespace macfw::fw1814::transport {

// Experimental 192-kHz host->FW1814 transmitter for the Linux S/PDIF-mode
// formation: 4 PCM + 1 MIDI (DBS=5), CIP_BLOCKING, 32 events in each
// data-bearing packet. The base-48 schedule carries data in three of every
// four bus cycles, with SYT offsets 0, 1024 and 2048 ticks.
class BlockingPcmTransmitRing192000 {
public:
    struct RefillResult {
        std::size_t packetsVisited = 0;
        std::size_t dataPacketsRefilled = 0;
        std::size_t framesRequested = 0;
        std::size_t framesFromBuffer = 0;
        std::size_t framesSilenced = 0;
        std::size_t nonzeroFrames = 0;
        std::int32_t peakSample = 0;
        std::uint64_t firstLoudHostTime = 0;
    };

    BlockingPcmTransmitRing192000() = default;
    ~BlockingPcmTransmitRing192000() { reset(); }
    BlockingPcmTransmitRing192000(const BlockingPcmTransmitRing192000&) = delete;
    BlockingPcmTransmitRing192000& operator=(const BlockingPcmTransmitRing192000&) = delete;

    BlockingPcmTransmitRing192000(BlockingPcmTransmitRing192000&& other) noexcept {
        moveFrom(std::move(other));
    }
    BlockingPcmTransmitRing192000& operator=(BlockingPcmTransmitRing192000&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(std::move(other));
        }
        return *this;
    }

    static BlockingPcmTransmitRing192000 create(
        FireWireDevice& device,
        UInt32 firstCycle,
        std::size_t packetCount = kRequiredPackets) {
        BlockingPcmTransmitRing192000 ring;
        auto native = device.nativeHandle();
        if (!native || (packetCount != kRequiredPackets &&
                        packetCount != kExtendedPackets &&
                        packetCount != kLongPackets))
            return ring;

        ring.packetCount_ = packetCount;
        ring.firstCycle_ = firstCycle % kCyclesPerSecond;
        ring.mappedBytes_ = sizeof(StorageSlot) * packetCount;
        ring.storage_ = static_cast<StorageSlot*>(mmap(
            nullptr, ring.mappedBytes_, PROT_READ | PROT_WRITE,
            MAP_ANON | MAP_SHARED, -1, 0));
        if (ring.storage_ == MAP_FAILED) {
            ring.storage_ = nullptr;
            ring.reset();
            return ring;
        }
        std::memset(ring.storage_, 0, ring.mappedBytes_);

        macfw::am824::Playback192kState state{};
        UInt32 cycle = ring.firstCycle_;
        for (std::size_t i = 0; i < packetCount; ++i, ++cycle) {
            auto packet = buildSilencePacket(cycle % kCyclesPerSecond, state);
            auto& slot = ring.storage_[i];
            slot.length = packet.length;
            slot.dataBearing = packet.dataBearing;
            std::copy_n(packet.bytes, packet.length, slot.payload);
        }

        // Both supported ring lengths are divisible by the four-cycle 192-kHz
        // cadence. Verify that each immutable NuDCL length repeats on reuse.
        {
            macfw::am824::Playback192kState verifyState = state;
            UInt32 verifyCycle = cycle;
            for (std::size_t i = 0; i < packetCount; ++i, ++verifyCycle) {
                const auto packet = buildSilencePacket(
                    verifyCycle % kCyclesPerSecond, verifyState);
                if (packet.length != ring.storage_[i].length ||
                    packet.dataBearing != ring.storage_[i].dataBearing) {
                    ring.reset();
                    return ring;
                }
            }
        }

        ring.pool_ = (*native)->CreateNuDCLPool(
            native, static_cast<UInt32>(packetCount),
            CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
        if (!ring.pool_) {
            ring.reset();
            return ring;
        }
        (*ring.pool_)->SetCurrentTagAndSync(ring.pool_, 1, 0);

        NuDCLRef first = nullptr;
        NuDCLRef last = nullptr;
        for (std::size_t i = 0; i < packetCount; ++i) {
            IOVirtualRange range = {
                reinterpret_cast<IOVirtualAddress>(ring.storage_[i].payload),
                ring.storage_[i].length
            };
            auto dcl = (*ring.pool_)->AllocateSendPacket(ring.pool_, nullptr, 1, &range);
            if (!dcl) {
                ring.reset();
                return ring;
            }
            const NuDCLRef ref = reinterpret_cast<NuDCLRef>(dcl);
            if (!first) first = ref;
            last = ref;
        }

        if (!first || !last ||
            (*ring.pool_)->SetDCLBranch(last, first) != kIOReturnSuccess) {
            ring.reset();
            return ring;
        }

        DCLCommand* program = (*ring.pool_)->GetProgram(ring.pool_);
        if (!program) {
            ring.reset();
            return ring;
        }

        IOVirtualRange mapped = {
            reinterpret_cast<IOVirtualAddress>(ring.storage_),
            static_cast<IOByteCount>(ring.mappedBytes_)
        };
        ring.localPort_ = (*native)->CreateLocalIsochPort(
            native, true, program,
            kFWDCLCycleEvent, ring.firstCycle_, 0x1fffu,
            nullptr, 0, &mapped, 1,
            CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
        if (!ring.localPort_)
            ring.reset();
        return ring;
    }

    RefillResult refill(macfw::PcmRingBuffer& pcm,
                        std::size_t firstPacket,
                        std::size_t packetCount,
                        macfw::am824::Playback192kState& state,
                        UInt32& cycle) {
        RefillResult result{};
        if (!storage_ || (packetCount_ != kRequiredPackets &&
                          packetCount_ != kExtendedPackets &&
                          packetCount_ != kLongPackets) || !pcm.valid() ||
            pcm.channelCount() != kPcmChannels || firstPacket >= packetCount_ ||
            packetCount == 0)
            return result;

        const std::size_t end = std::min(packetCount_, firstPacket + packetCount);
        std::int32_t frames[kEventsPerDataPacket * kPcmChannels]{};

        for (std::size_t i = firstPacket; i < end; ++i, ++cycle) {
            ++result.packetsVisited;
            auto packet = buildSilencePacket(cycle % kCyclesPerSecond, state);
            if (packet.length != storage_[i].length ||
                packet.dataBearing != storage_[i].dataBearing)
                return RefillResult{};

            if (packet.dataBearing) {
                const auto rr = pcm.read(frames, kEventsPerDataPacket);
                result.framesRequested += rr.framesRequested;
                result.framesFromBuffer += rr.framesFromBuffer;
                result.framesSilenced += rr.framesSilenced;
                if (result.firstLoudHostTime == 0) {
                    for (const auto sample : frames) {
                        if (sample >= 6291456 || sample <= -6291456) {
                            result.firstLoudHostTime = mach_absolute_time();
                            break;
                        }
                    }
                }

                for (std::size_t event = 0; event < kEventsPerDataPacket; ++event) {
                    bool nonzero = false;
                    for (std::size_t ch = 0; ch < kPcmChannels; ++ch) {
                        const auto sample = std::max<std::int32_t>(
                            -8388608,
                            std::min<std::int32_t>(
                                8388607, frames[event * kPcmChannels + ch]));
                        nonzero = nonzero || sample != 0;
                        const auto magnitude = sample < 0 ? -sample : sample;
                        result.peakSample = std::max(result.peakSample, magnitude);
                        const std::uint32_t word = 0x40000000u |
                            (static_cast<std::uint32_t>(sample) & 0x00ffffffu);
                        const std::size_t off = 8 +
                            (event * kDbs + ch) * sizeof(std::uint32_t);
                        putBe32(packet.bytes + off, word);
                    }
                    result.nonzeroFrames += nonzero;
                }
                ++result.dataPacketsRefilled;
            }

            auto& slot = storage_[i];
            std::copy_n(packet.bytes, packet.length, slot.payload);
        }
        std::atomic_thread_fence(std::memory_order_release);
        return result;
    }

    // Advance packet cadence without touching live DMA slots.
    void advanceState(macfw::am824::Playback192kState& state,
                      UInt32& cycle, std::size_t packets) const {
        for (std::size_t i = 0; i < packets; ++i, ++cycle)
            buildSilencePacket(cycle % kCyclesPerSecond, state);
    }

    explicit operator bool() const { return localPort_ != nullptr; }
    IOFireWireLibLocalIsochPortRef nativeLocalPort() const { return localPort_; }
    UInt32 firstCycle() const { return firstCycle_; }
    std::size_t packetCount() const { return packetCount_; }

    static constexpr std::size_t pcmChannels() { return kPcmChannels; }
    static constexpr UInt32 maxPacketBytes() { return kMaxPacketBytes; }
    static constexpr std::size_t requiredPackets() { return kRequiredPackets; }

private:
    static constexpr UInt32 kCyclesPerSecond = 8000;
    static constexpr std::size_t kPcmChannels = 4;
    static constexpr std::size_t kDbs = 5;
    static constexpr std::size_t kEventsPerDataPacket = 32;
    static constexpr std::size_t kRequiredPackets = 640;
    static constexpr std::size_t kExtendedPackets = 1280;
    static constexpr std::size_t kLongPackets = 2560;
    static constexpr UInt32 kMaxPacketBytes =
        8 + kEventsPerDataPacket * kDbs * sizeof(std::uint32_t);

    struct PacketImage {
        std::uint8_t bytes[kMaxPacketBytes]{};
        UInt32 length = 0;
        bool dataBearing = false;
    };

    struct StorageSlot {
        UInt32 length = 0;
        bool dataBearing = false;
        std::uint8_t payload[kMaxPacketBytes]{};
    };

    static void putBe32(std::uint8_t* p, std::uint32_t v) {
        p[0] = static_cast<std::uint8_t>((v >> 24) & 0xffu);
        p[1] = static_cast<std::uint8_t>((v >> 16) & 0xffu);
        p[2] = static_cast<std::uint8_t>((v >> 8) & 0xffu);
        p[3] = static_cast<std::uint8_t>(v & 0xffu);
    }

    static PacketImage buildSilencePacket(
        UInt32 txCycle, macfw::am824::Playback192kState& state) {
        PacketImage packet{};
        const auto schedule = macfw::am824::nextPlayback192kCycle(state);
        packet.dataBearing = schedule.dataBearing;

        putBe32(packet.bytes,
                (static_cast<std::uint32_t>(kDbs) << 16) | schedule.dbc);

        if (!packet.dataBearing) {
            putBe32(packet.bytes + 4, 0x9006ffffu);
            packet.length = 8;
        } else {
            const std::uint16_t syt = macfw::am824::computePlaybackSyt(
                txCycle, schedule.sytOffset,
                macfw::am824::kPlayback48kTransferDelayTicks);
            putBe32(packet.bytes + 4,
                    0x90060000u | static_cast<std::uint32_t>(syt));

            std::size_t offset = 8;
            for (std::size_t event = 0; event < kEventsPerDataPacket; ++event) {
                for (std::size_t ch = 0; ch < kPcmChannels; ++ch) {
                    putBe32(packet.bytes + offset, 0x40000000u);
                    offset += 4;
                }
                putBe32(packet.bytes + offset, 0x80000000u);
                offset += 4;
            }
            packet.length = static_cast<UInt32>(offset);
        }
        return packet;
    }

    void reset() {
        if (localPort_) {
            (*localPort_)->Release(localPort_);
            localPort_ = nullptr;
        }
        if (pool_) {
            (*pool_)->Release(pool_);
            pool_ = nullptr;
        }
        if (storage_) {
            munmap(storage_, mappedBytes_);
            storage_ = nullptr;
        }
        packetCount_ = 0;
        mappedBytes_ = 0;
        firstCycle_ = 0;
    }

    void moveFrom(BlockingPcmTransmitRing192000&& other) noexcept {
        storage_ = other.storage_;
        packetCount_ = other.packetCount_;
        mappedBytes_ = other.mappedBytes_;
        firstCycle_ = other.firstCycle_;
        pool_ = other.pool_;
        localPort_ = other.localPort_;
        other.storage_ = nullptr;
        other.packetCount_ = 0;
        other.mappedBytes_ = 0;
        other.firstCycle_ = 0;
        other.pool_ = nullptr;
        other.localPort_ = nullptr;
    }

    StorageSlot* storage_ = nullptr;
    std::size_t packetCount_ = 0;
    std::size_t mappedBytes_ = 0;
    UInt32 firstCycle_ = 0;
    IOFireWireLibNuDCLPoolRef pool_ = nullptr;
    IOFireWireLibLocalIsochPortRef localPort_ = nullptr;
};

class BlockingPcmStream192000 {
public:
    struct Stats {
        std::uint64_t halvesRefilled = 0;
        std::uint64_t rollingRefills = 0;
        std::uint64_t rollingPacketsRefilled = 0;
        std::uint64_t rollingDeadlineMisses = 0;
        std::uint64_t dataPacketsRefilled = 0;
        std::uint64_t framesFromBuffer = 0;
        std::uint64_t framesSilenced = 0;
        std::uint64_t nonzeroFrames = 0;
        std::int32_t peakSample = 0;
        std::uint64_t firstLoudHostTime = 0;
        std::uint64_t lateCyclePolls = 0;
        std::uint64_t dangerousCyclePolls = 0;
        UInt32 maxCycleDelta = 0;
        std::uint64_t maxHalvesBehind = 0;
    };

    BlockingPcmStream192000(BlockingPcmTransmitRing192000& tx,
                            macfw::PcmRingBuffer& pcm,
                            UInt32 observedCycleBeforeStart,
                            UInt32 firstTxCycle,
                            std::size_t halfPackets = 320)
        : tx_(&tx), pcm_(&pcm),
          initialCycle_(observedCycleBeforeStart % kCyclesPerSecond),
          firstTxCycle_(firstTxCycle % kCyclesPerSecond),
          leadCycles_(cycleDelta(firstTxCycle_, initialCycle_)),
          lastCycle_(initialCycle_), nextCycle_(firstTxCycle_),
          halfPackets_(halfPackets) {}

    bool valid() const {
        return tx_ && pcm_ && static_cast<bool>(*tx_) && pcm_->valid() &&
               pcm_->channelCount() == BlockingPcmTransmitRing192000::pcmChannels() &&
               (tx_->packetCount() == 640 || tx_->packetCount() == 1280 ||
                tx_->packetCount() == 2560) &&
               tx_->packetCount() == halfPackets_ * 2;
    }

    bool enableRolling(std::size_t leadPackets, std::size_t guardPackets) {
        if (!valid() || leadPackets == 0 ||
            leadPackets >= tx_->packetCount() || guardPackets == 0 ||
            guardPackets >= leadPackets)
            return false;
        rollingEnabled_ = true;
        rollingLeadPackets_ = leadPackets;
        rollingGuardPackets_ = guardPackets;
        return true;
    }

    bool prime() {
        if (!valid()) return false;
        macfw::am824::Playback192kState state{};
        UInt32 cycle = firstTxCycle_;
        const auto first = tx_->refill(*pcm_, 0, halfPackets_, state, cycle);
        const auto second = tx_->refill(*pcm_, halfPackets_, halfPackets_, state, cycle);
        if (first.dataPacketsRefilled == 0 || second.dataPacketsRefilled == 0)
            return false;
        nextState_ = state;
        nextCycle_ = cycle;
        primed_ = true;
        return true;
    }

    void service(UInt32 currentCycle) {
        if (!valid() || !primed_) return;
        currentCycle %= kCyclesPerSecond;
        const UInt32 delta = cycleDelta(currentCycle, lastCycle_);
        lastCycle_ = currentCycle;
        if (delta > 32) ++stats_.lateCyclePolls;
        if (delta >= halfPackets_) ++stats_.dangerousCyclePolls;
        stats_.maxCycleDelta = std::max(stats_.maxCycleDelta, delta);
        cyclesObserved_ += delta;

        if (!streamReached_) {
            if (cyclesObserved_ < leadCycles_) return;
            streamReached_ = true;
        }

        const std::uint64_t sinceStart = cyclesObserved_ - leadCycles_;
        if (rollingEnabled_) {
            serviceRolling(sinceStart);
            return;
        }
        const std::uint64_t halfNumber = sinceStart / halfPackets_;
        stats_.maxHalvesBehind = std::max(
            stats_.maxHalvesBehind, halfNumber - lastHalfNumber_);
        while (lastHalfNumber_ < halfNumber) {
            const std::size_t consumedHalf =
                static_cast<std::size_t>(lastHalfNumber_ & 1u);
            const auto rr = tx_->refill(*pcm_, consumedHalf * halfPackets_,
                                        halfPackets_, nextState_, nextCycle_);
            if (rr.dataPacketsRefilled == 0)
                return;
            ++stats_.halvesRefilled;
            stats_.dataPacketsRefilled += rr.dataPacketsRefilled;
            stats_.framesFromBuffer += rr.framesFromBuffer;
            stats_.framesSilenced += rr.framesSilenced;
            stats_.nonzeroFrames += rr.nonzeroFrames;
            stats_.peakSample = std::max(stats_.peakSample, rr.peakSample);
            if (stats_.firstLoudHostTime == 0 && rr.firstLoudHostTime != 0)
                stats_.firstLoudHostTime = rr.firstLoudHostTime;
            ++lastHalfNumber_;
        }
    }

    const Stats& stats() const { return stats_; }
    bool streamReached() const { return streamReached_; }
    bool healthy() const { return rollingHealthy_; }

private:
    static constexpr UInt32 kCyclesPerSecond = 8000;

    static UInt32 cycleDelta(UInt32 newer, UInt32 older) {
        return (newer + kCyclesPerSecond - older) % kCyclesPerSecond;
    }

    bool refillRolling(std::uint64_t firstPacket, std::size_t packetCount) {
        while (packetCount != 0) {
            const std::size_t slot = static_cast<std::size_t>(
                firstPacket % tx_->packetCount());
            const std::size_t chunk = std::min(
                packetCount, tx_->packetCount() - slot);
            const auto refill = tx_->refill(*pcm_, slot, chunk,
                                            nextState_, nextCycle_);
            if (refill.packetsVisited != chunk)
                return false;
            ++stats_.rollingRefills;
            stats_.rollingPacketsRefilled += refill.packetsVisited;
            stats_.dataPacketsRefilled += refill.dataPacketsRefilled;
            stats_.framesFromBuffer += refill.framesFromBuffer;
            stats_.framesSilenced += refill.framesSilenced;
            stats_.nonzeroFrames += refill.nonzeroFrames;
            stats_.peakSample = std::max(stats_.peakSample, refill.peakSample);
            if (stats_.firstLoudHostTime == 0 && refill.firstLoudHostTime != 0)
                stats_.firstLoudHostTime = refill.firstLoudHostTime;
            firstPacket += chunk;
            packetCount -= chunk;
        }
        return true;
    }

    void serviceRolling(std::uint64_t sinceStart) {
        if (!rollingInitialized_) {
            rollingNextPacket_ = sinceStart + rollingLeadPackets_;
            nextState_ = {};
            nextCycle_ = firstTxCycle_;
            tx_->advanceState(nextState_, nextCycle_,
                              static_cast<std::size_t>(rollingNextPacket_));
            rollingInitialized_ = true;
        }
        const std::uint64_t safeFloor = sinceStart + rollingGuardPackets_;
        if (rollingNextPacket_ < safeFloor) {
            ++stats_.rollingDeadlineMisses;
            rollingHealthy_ = false;
            return;
        }
        const std::uint64_t targetExclusive =
            sinceStart + rollingLeadPackets_ + 1;
        if (rollingNextPacket_ >= targetExclusive)
            return;
        const auto count = static_cast<std::size_t>(
            targetExclusive - rollingNextPacket_);
        if (!refillRolling(rollingNextPacket_, count)) {
            ++stats_.rollingDeadlineMisses;
            rollingHealthy_ = false;
            return;
        }
        rollingNextPacket_ = targetExclusive;
    }

    BlockingPcmTransmitRing192000* tx_ = nullptr;
    macfw::PcmRingBuffer* pcm_ = nullptr;
    UInt32 initialCycle_ = 0;
    UInt32 firstTxCycle_ = 0;
    UInt32 leadCycles_ = 0;
    UInt32 lastCycle_ = 0;
    UInt32 nextCycle_ = 0;
    std::size_t halfPackets_ = 0;
    macfw::am824::Playback192kState nextState_{};
    std::uint64_t cyclesObserved_ = 0;
    std::uint64_t lastHalfNumber_ = 0;
    std::uint64_t rollingNextPacket_ = 0;
    std::size_t rollingLeadPackets_ = 0;
    std::size_t rollingGuardPackets_ = 0;
    bool rollingEnabled_ = false;
    bool rollingInitialized_ = false;
    bool rollingHealthy_ = true;
    bool primed_ = false;
    bool streamReached_ = false;
    Stats stats_{};
};

} // namespace macfw::fw1814::transport
