#pragma once

#include "macfw/am824_playback.h"
#include "macfw/firewire_device.h"
#include "macfw/pcm_ring_buffer.h"

#include <IOKit/firewire/IOFireWireLibIsoch.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <utility>

namespace macfw::fw1814::transport {

// Native 44.1-kHz host->FW1814 transmitter for the hardware-known S/PDIF-mode
// formation: 6 PCM + 1 MIDI (DBS=7), CIP_BLOCKING, eight events in each
// data-bearing packet. Data/NODATA placement and SYT follow Linux's base-44.1
// blocking AMDTP state exactly via macfw::am824::Playback44100State.
//
// A 640-packet ring is mandatory: 44.1 kHz / 8 events / 8000 cycles reduces to
// 441 data packets per 640 bus cycles, so the data/NODATA *length* pattern is
// stable at the NuDCL wrap. DBC and SYT do not repeat there; refill() rebuilds
// the complete packet headers with continuation state before each half is reused.
class BlockingPcmTransmitRing44100 {
public:
    struct RefillResult {
        std::size_t packetsVisited = 0;
        std::size_t dataPacketsRefilled = 0;
        std::size_t framesRequested = 0;
        std::size_t framesFromBuffer = 0;
        std::size_t framesSilenced = 0;
    };

    BlockingPcmTransmitRing44100() = default;
    ~BlockingPcmTransmitRing44100() { reset(); }
    BlockingPcmTransmitRing44100(const BlockingPcmTransmitRing44100&) = delete;
    BlockingPcmTransmitRing44100& operator=(const BlockingPcmTransmitRing44100&) = delete;

    BlockingPcmTransmitRing44100(BlockingPcmTransmitRing44100&& other) noexcept {
        moveFrom(std::move(other));
    }
    BlockingPcmTransmitRing44100& operator=(BlockingPcmTransmitRing44100&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(std::move(other));
        }
        return *this;
    }

    static BlockingPcmTransmitRing44100 create(FireWireDevice& device,
                                                UInt32 firstCycle,
                                                std::size_t packetCount = kRequiredPackets) {
        BlockingPcmTransmitRing44100 ring;
        auto native = device.nativeHandle();
        if (!native || packetCount != kRequiredPackets)
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

        macfw::am824::Playback44100State state{};
        UInt32 cycle = ring.firstCycle_;
        for (std::size_t i = 0; i < packetCount; ++i, ++cycle) {
            auto packet = buildSilencePacket(cycle % kCyclesPerSecond, state);
            auto& slot = ring.storage_[i];
            slot.length = packet.length;
            slot.dataBearing = packet.dataBearing;
            std::copy_n(packet.bytes, packet.length, slot.payload);
        }

        // Verify the one-ring continuation preserves every slot's immutable
        // NuDCL transfer length. Header contents themselves intentionally move
        // forward in DBC/SYT and are rebuilt dynamically during refill.
        {
            macfw::am824::Playback44100State verifyState = state;
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
                        macfw::am824::Playback44100State& state,
                        UInt32& cycle) {
        RefillResult result{};
        if (!storage_ || packetCount_ != kRequiredPackets || !pcm.valid() ||
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

                for (std::size_t event = 0; event < kEventsPerDataPacket; ++event) {
                    for (std::size_t ch = 0; ch < kPcmChannels; ++ch) {
                        const auto sample = std::max<std::int32_t>(
                            -8388608,
                            std::min<std::int32_t>(
                                8388607, frames[event * kPcmChannels + ch]));
                        const std::uint32_t word = 0x40000000u |
                            (static_cast<std::uint32_t>(sample) & 0x00ffffffu);
                        const std::size_t off = 8 +
                            (event * kDbs + ch) * sizeof(std::uint32_t);
                        putBe32(packet.bytes + off, word);
                    }
                }
                ++result.dataPacketsRefilled;
            }

            std::copy_n(packet.bytes, packet.length, storage_[i].payload);
        }

        std::atomic_thread_fence(std::memory_order_release);
        return result;
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
    static constexpr std::size_t kPcmChannels = 6;
    static constexpr std::size_t kDbs = 7;
    static constexpr std::size_t kEventsPerDataPacket = 8;
    static constexpr std::size_t kRequiredPackets = 640;
    static constexpr UInt32 kMaxPacketBytes =
        8 + kEventsPerDataPacket * kDbs * sizeof(std::uint32_t); // 232

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
        UInt32 txCycle, macfw::am824::Playback44100State& state) {
        PacketImage packet{};
        const std::uint8_t dbc = state.dbc;
        const std::uint32_t sytOffset =
            macfw::am824::nextPlayback44100SytOffset(state);
        packet.dataBearing = sytOffset < macfw::am824::kTicksPerCycle;

        putBe32(packet.bytes,
                (static_cast<std::uint32_t>(kDbs) << 16) | dbc);

        if (!packet.dataBearing) {
            // AM824 FDF base-frequency code 1 = 44.1 kHz; no SYT on NODATA.
            putBe32(packet.bytes + 4, 0x9001ffffu);
            packet.length = 8;
            return packet;
        }

        const std::uint16_t syt = macfw::am824::computePlaybackSyt(
            txCycle, sytOffset, macfw::am824::kPlayback44100TransferDelayTicks);
        putBe32(packet.bytes + 4,
                0x90010000u | static_cast<std::uint32_t>(syt));

        std::size_t offset = 8;
        for (std::size_t event = 0; event < kEventsPerDataPacket; ++event) {
            for (std::size_t ch = 0; ch < kPcmChannels; ++ch) {
                putBe32(packet.bytes + offset, 0x40000000u);
                offset += 4;
            }
            putBe32(packet.bytes + offset, 0x80000000u); // MIDI no-data.
            offset += 4;
        }
        packet.length = static_cast<UInt32>(offset);
        state.dbc = static_cast<std::uint8_t>(state.dbc + kEventsPerDataPacket);
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

    void moveFrom(BlockingPcmTransmitRing44100&& other) noexcept {
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

class BlockingPcmStream44100 {
public:
    struct Stats {
        std::uint64_t halvesRefilled = 0;
        std::uint64_t dataPacketsRefilled = 0;
        std::uint64_t framesFromBuffer = 0;
        std::uint64_t framesSilenced = 0;
        std::uint64_t lateCyclePolls = 0;
    };

    BlockingPcmStream44100(BlockingPcmTransmitRing44100& tx,
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
               pcm_->channelCount() == BlockingPcmTransmitRing44100::pcmChannels() &&
               tx_->packetCount() == 640 && halfPackets_ == 320;
    }

    bool prime() {
        if (!valid()) return false;
        macfw::am824::Playback44100State state{};
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
        cyclesObserved_ += delta;

        if (!streamReached_) {
            if (cyclesObserved_ < leadCycles_) return;
            streamReached_ = true;
        }

        const std::uint64_t sinceStart = cyclesObserved_ - leadCycles_;
        const std::uint64_t halfNumber = sinceStart / halfPackets_;
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
            ++lastHalfNumber_;
        }
    }

    const Stats& stats() const { return stats_; }
    bool streamReached() const { return streamReached_; }

private:
    static constexpr UInt32 kCyclesPerSecond = 8000;

    static UInt32 cycleDelta(UInt32 newer, UInt32 older) {
        return (newer + kCyclesPerSecond - older) % kCyclesPerSecond;
    }

    BlockingPcmTransmitRing44100* tx_ = nullptr;
    macfw::PcmRingBuffer* pcm_ = nullptr;
    UInt32 initialCycle_ = 0;
    UInt32 firstTxCycle_ = 0;
    UInt32 leadCycles_ = 0;
    UInt32 lastCycle_ = 0;
    UInt32 nextCycle_ = 0;
    std::size_t halfPackets_ = 0;
    macfw::am824::Playback44100State nextState_{};
    std::uint64_t cyclesObserved_ = 0;
    std::uint64_t lastHalfNumber_ = 0;
    bool primed_ = false;
    bool streamReached_ = false;
    Stats stats_{};
};

} // namespace macfw::fw1814::transport
