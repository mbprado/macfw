#pragma once

#include "macfw/firewire_device.h"
#include <IOKit/firewire/IOFireWireLibIsoch.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <utility>

namespace macfw::fw1814 {

// Playback-mapping transmitter for the hardware-proven 48 kHz / S/PDIF
// FW1814 stream formation: 6 PCM + 1 MIDI (DBS=7), CIP_BLOCKING.
//
// It is intentionally separate from BlockingSilenceTransmitRing so the
// known-good bring-up diagnostic remains unchanged. One selected PCM position
// carries a 500 Hz, -24 dBFS-peak MBLA tone; all other PCM positions carry
// silence and the MIDI position carries AM824 MIDI no-data.
//
// 500 Hz is chosen because the 128-cycle DCL loop contains exactly 768 audio
// samples (96 data packets * 8 events), i.e. exactly eight 500 Hz periods at
// 48 kHz. The loop therefore has no waveform discontinuity at its wrap.
class BlockingToneTransmitRing {
public:
    BlockingToneTransmitRing() = default;
    ~BlockingToneTransmitRing() { reset(); }

    BlockingToneTransmitRing(const BlockingToneTransmitRing&) = delete;
    BlockingToneTransmitRing& operator=(const BlockingToneTransmitRing&) = delete;

    BlockingToneTransmitRing(BlockingToneTransmitRing&& other) noexcept {
        moveFrom(std::move(other));
    }

    BlockingToneTransmitRing& operator=(BlockingToneTransmitRing&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(std::move(other));
        }
        return *this;
    }

    static BlockingToneTransmitRing create48k(FireWireDevice& device,
                                                UInt32 firstCycle,
                                                std::uint8_t tonePosition,
                                                std::size_t packetCount = 128) {
        BlockingToneTransmitRing ring;
        auto native = device.nativeHandle();
        if (!native || tonePosition >= kPcmChannels || packetCount == 0 ||
            (packetCount % 128) != 0)
            return ring;

        ring.packetCount_ = packetCount;
        ring.firstCycle_ = firstCycle % kCyclesPerSecond;
        ring.tonePosition_ = tonePosition;
        ring.mappedBytes_ = sizeof(StorageSlot) * packetCount;
        ring.storage_ = static_cast<StorageSlot*>(
            mmap(nullptr, ring.mappedBytes_, PROT_READ | PROT_WRITE,
                 MAP_ANON | MAP_SHARED, -1, 0));
        if (ring.storage_ == MAP_FAILED) {
            ring.storage_ = nullptr;
            ring.reset();
            return ring;
        }
        std::memset(ring.storage_, 0, ring.mappedBytes_);

        std::uint8_t dbc = 0;
        std::uint64_t sampleIndex = 0;
        for (std::size_t i = 0; i < packetCount; ++i) {
            const std::uint8_t phase = static_cast<std::uint8_t>(i & 3u);
            const UInt32 cycle = static_cast<UInt32>(
                (ring.firstCycle_ + i) % kCyclesPerSecond);
            auto* payload = ring.storage_[i].payload;

            putBe32(payload,
                    (static_cast<std::uint32_t>(kDbs) << 16) | dbc);

            if (phase == 3u) {
                putBe32(payload + 4, 0x9002ffffu);
                ring.storage_[i].length = 8;
                continue;
            }

            const std::uint32_t sytOffset =
                static_cast<std::uint32_t>(phase) * 1024u;
            const std::uint16_t syt = computeSyt(cycle, sytOffset);
            putBe32(payload + 4,
                    0x90020000u | static_cast<std::uint32_t>(syt));

            std::size_t offset = 8;
            for (std::size_t event = 0; event < 8; ++event) {
                const double phaseRadians =
                    kTwoPi * kToneHz * static_cast<double>(sampleIndex) /
                    static_cast<double>(kSampleRate);
                const auto sample = static_cast<std::int32_t>(
                    std::lround(std::sin(phaseRadians) * kTonePeak));
                const std::uint32_t toneWord = encodeMbla24(sample);

                for (std::size_t pos = 0; pos < kPcmChannels; ++pos) {
                    putBe32(payload + offset,
                            pos == tonePosition ? toneWord : 0x40000000u);
                    offset += 4;
                }

                // Last DBS position is MIDI.
                putBe32(payload + offset, 0x80000000u);
                offset += 4;
                ++sampleIndex;
            }

            ring.storage_[i].length = static_cast<UInt32>(offset);
            dbc = static_cast<std::uint8_t>(dbc + 8u);
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
            auto dcl = (*ring.pool_)->AllocateSendPacket(
                ring.pool_, nullptr, 1, &range);
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
        if (!ring.localPort_) {
            ring.reset();
            return ring;
        }

        return ring;
    }

    explicit operator bool() const { return localPort_ != nullptr; }
    IOFireWireLibLocalIsochPortRef nativeLocalPort() const { return localPort_; }
    UInt32 firstCycle() const { return firstCycle_; }
    std::size_t packetCount() const { return packetCount_; }
    std::uint8_t tonePosition() const { return tonePosition_; }

private:
    static constexpr UInt32 kCyclesPerSecond = 8000;
    static constexpr std::uint8_t kDbs = 7;
    static constexpr std::uint8_t kPcmChannels = 6;
    static constexpr UInt32 kSampleRate = 48000;
    static constexpr double kToneHz = 500.0;
    static constexpr double kTonePeak = 529285.0; // -24 dBFS peak in signed 24-bit.
    static constexpr double kTwoPi = 6.283185307179586476925286766559;
    static constexpr std::size_t kMaxPacketBytes = 8 + 8 * kDbs * 4;
    static constexpr std::uint32_t kTicksPerCycle = 3072u;
    static constexpr std::uint32_t kTicksPerSecond = 24576000u;
    static constexpr std::uint32_t kTransferDelayTicks =
        0x2e00u - kTicksPerCycle + (kTicksPerSecond * 8u / kSampleRate);

    struct StorageSlot {
        UInt32 length = 0;
        std::uint8_t payload[kMaxPacketBytes]{};
    };

    static std::uint32_t encodeMbla24(std::int32_t sample) {
        const auto payload = static_cast<std::uint32_t>(sample) & 0x00ffffffu;
        return 0x40000000u | payload;
    }

    static void putBe32(std::uint8_t* p, std::uint32_t v) {
        p[0] = static_cast<std::uint8_t>((v >> 24) & 0xffu);
        p[1] = static_cast<std::uint8_t>((v >> 16) & 0xffu);
        p[2] = static_cast<std::uint8_t>((v >> 8) & 0xffu);
        p[3] = static_cast<std::uint8_t>(v & 0xffu);
    }

    static std::uint16_t computeSyt(UInt32 cycle, std::uint32_t sytOffsetTicks) {
        const std::uint32_t total = sytOffsetTicks + kTransferDelayTicks;
        const std::uint32_t presentationCycle =
            cycle + total / kTicksPerCycle;
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
        tonePosition_ = 0;
    }

    void moveFrom(BlockingToneTransmitRing&& other) noexcept {
        storage_ = other.storage_;
        packetCount_ = other.packetCount_;
        mappedBytes_ = other.mappedBytes_;
        firstCycle_ = other.firstCycle_;
        tonePosition_ = other.tonePosition_;
        pool_ = other.pool_;
        localPort_ = other.localPort_;

        other.storage_ = nullptr;
        other.packetCount_ = 0;
        other.mappedBytes_ = 0;
        other.firstCycle_ = 0;
        other.tonePosition_ = 0;
        other.pool_ = nullptr;
        other.localPort_ = nullptr;
    }

    StorageSlot* storage_ = nullptr;
    std::size_t packetCount_ = 0;
    std::size_t mappedBytes_ = 0;
    UInt32 firstCycle_ = 0;
    std::uint8_t tonePosition_ = 0;
    IOFireWireLibNuDCLPoolRef pool_ = nullptr;
    IOFireWireLibLocalIsochPortRef localPort_ = nullptr;
};

} // namespace macfw::fw1814
