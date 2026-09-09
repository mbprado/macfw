#pragma once

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

// Live 48-kHz host->FW1814 transmitter for the hardware-proven S/PDIF-mode
// formation: 6 PCM + 1 MIDI (DBS=7), CIP_BLOCKING, 8/8/8/NODATA cadence.
class BlockingPcmTransmitRing48k {
public:
    struct RefillResult {
        std::size_t packetsVisited = 0;
        std::size_t dataPacketsRefilled = 0;
        std::size_t framesRequested = 0;
        std::size_t framesFromBuffer = 0;
        std::size_t framesSilenced = 0;
    };

    BlockingPcmTransmitRing48k() = default;
    ~BlockingPcmTransmitRing48k() { reset(); }
    BlockingPcmTransmitRing48k(const BlockingPcmTransmitRing48k&) = delete;
    BlockingPcmTransmitRing48k& operator=(const BlockingPcmTransmitRing48k&) = delete;

    BlockingPcmTransmitRing48k(BlockingPcmTransmitRing48k&& other) noexcept {
        moveFrom(std::move(other));
    }
    BlockingPcmTransmitRing48k& operator=(BlockingPcmTransmitRing48k&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(std::move(other));
        }
        return *this;
    }

    static BlockingPcmTransmitRing48k create(FireWireDevice& device,
                                              UInt32 firstCycle,
                                              std::size_t packetCount = 128) {
        BlockingPcmTransmitRing48k ring;
        auto native = device.nativeHandle();
        if (!native || packetCount == 0 || (packetCount & 3u) != 0)
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

        std::uint8_t dbc = 0;
        for (std::size_t i = 0; i < packetCount; ++i) {
            const std::uint8_t phase = static_cast<std::uint8_t>(i & 3u);
            const UInt32 cycle = static_cast<UInt32>(
                (ring.firstCycle_ + i) % kCyclesPerSecond);
            auto& slot = ring.storage_[i];
            auto* payload = slot.payload;

            putBe32(payload, (static_cast<std::uint32_t>(kDbs) << 16) | dbc);
            slot.dataBearing = phase != 3u;

            if (!slot.dataBearing) {
                putBe32(payload + 4, 0x9002ffffu);
                slot.length = 8;
                continue;
            }

            const std::uint32_t sytOffset =
                static_cast<std::uint32_t>(phase) * 1024u;
            const std::uint16_t syt = computeSyt(cycle, sytOffset);
            putBe32(payload + 4, 0x90020000u | static_cast<std::uint32_t>(syt));

            std::size_t offset = 8;
            for (std::size_t event = 0; event < kEventsPerDataPacket; ++event) {
                for (std::size_t ch = 0; ch < kPcmChannels; ++ch) {
                    putBe32(payload + offset, 0x40000000u);
                    offset += 4;
                }
                putBe32(payload + offset, 0x80000000u); // MIDI no-data
                offset += 4;
            }
            slot.length = static_cast<UInt32>(offset);
            dbc = static_cast<std::uint8_t>(dbc + kEventsPerDataPacket);
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
                        std::size_t packetCount) {
        RefillResult result{};
        if (!storage_ || packetCount_ == 0 || !pcm.valid() ||
            pcm.channelCount() != kPcmChannels || firstPacket >= packetCount_ ||
            packetCount == 0)
            return result;

        const std::size_t end = std::min(packetCount_, firstPacket + packetCount);
        std::int32_t frames[kEventsPerDataPacket * kPcmChannels]{};
        for (std::size_t i = firstPacket; i < end; ++i) {
            ++result.packetsVisited;
            if (!storage_[i].dataBearing) continue;

            const auto rr = pcm.read(frames, kEventsPerDataPacket);
            result.framesRequested += rr.framesRequested;
            result.framesFromBuffer += rr.framesFromBuffer;
            result.framesSilenced += rr.framesSilenced;

            for (std::size_t event = 0; event < kEventsPerDataPacket; ++event) {
                for (std::size_t ch = 0; ch < kPcmChannels; ++ch) {
                    const auto sample = std::max<std::int32_t>(
                        -8388608, std::min<std::int32_t>(
                            8388607, frames[event * kPcmChannels + ch]));
                    const std::uint32_t word = 0x40000000u |
                        (static_cast<std::uint32_t>(sample) & 0x00ffffffu);
                    const std::size_t off = 8 +
                        (event * kDbs + ch) * sizeof(std::uint32_t);
                    putBe32(storage_[i].payload + off, word);
                }
            }
            ++result.dataPacketsRefilled;
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

private:
    static constexpr UInt32 kCyclesPerSecond = 8000;
    static constexpr std::size_t kPcmChannels = 6;
    static constexpr std::size_t kDbs = 7;
    static constexpr std::size_t kEventsPerDataPacket = 8;
    static constexpr UInt32 kMaxPacketBytes =
        8 + kEventsPerDataPacket * kDbs * sizeof(std::uint32_t); // 232
    static constexpr std::uint32_t kTicksPerCycle = 3072u;
    static constexpr std::uint32_t kTicksPerSecond = 24576000u;
    static constexpr std::uint32_t kTransferDelayTicks =
        0x2e00u - kTicksPerCycle + (kTicksPerSecond * 8u / 48000u);

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

    static std::uint16_t computeSyt(UInt32 cycle, std::uint32_t sytOffsetTicks) {
        const std::uint32_t total = sytOffsetTicks + kTransferDelayTicks;
        const std::uint32_t presentationCycle = cycle + total / kTicksPerCycle;
        const std::uint32_t presentationOffset = total % kTicksPerCycle;
        return static_cast<std::uint16_t>(
            ((presentationCycle & 0x0fu) << 12) | presentationOffset);
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

    void moveFrom(BlockingPcmTransmitRing48k&& other) noexcept {
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

} // namespace macfw::fw1814::transport
