#pragma once
#include "macfw/am824_playback.h"
#include "macfw/firewire_device.h"
#include "macfw/pcm_buffer.h"
#include "macfw/pcm_ring_buffer.h"
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace macfw {
class AmdtpTransmitRing {
public:
    struct PacketSlot {
        const std::uint8_t* payload = nullptr;
        std::size_t length = 0;
        UInt32 cycle = 0;
        std::uint8_t dbc = 0;
        std::uint16_t syt = 0xffffu;
        bool dataBearing = false;
    };

    struct RefillResult {
        std::size_t packetsVisited = 0;
        std::size_t dataPacketsRefilled = 0;
        std::size_t framesRequested = 0;
        std::size_t framesFromBuffer = 0;
        std::size_t framesSilenced = 0;
    };

    AmdtpTransmitRing() = default;
    ~AmdtpTransmitRing();
    AmdtpTransmitRing(const AmdtpTransmitRing&) = delete;
    AmdtpTransmitRing& operator=(const AmdtpTransmitRing&) = delete;
    AmdtpTransmitRing(AmdtpTransmitRing&& other) noexcept;
    AmdtpTransmitRing& operator=(AmdtpTransmitRing&& other) noexcept;

    static AmdtpTransmitRing createSilence48k(FireWireDevice& device,
                                              UInt32 firstCycle,
                                              std::size_t packetCount = 128);
    static AmdtpTransmitRing createPcm48k(FireWireDevice& device,
                                          UInt32 firstCycle,
                                          const PcmBufferView& pcm,
                                          std::size_t packetCount = 128);
    static AmdtpTransmitRing createTone48k(FireWireDevice& device,
                                           UInt32 firstCycle,
                                           std::size_t pcmPosition,
                                           double frequencyHz = 1000.0,
                                           double amplitude = 131072.0,
                                           std::size_t packetCount = 128);

    static AmdtpTransmitRing createSilence44100(FireWireDevice& device,
                                                UInt32 firstCycle,
                                                std::size_t packetCount = 128);
    static AmdtpTransmitRing createPcm44100(FireWireDevice& device,
                                            UInt32 firstCycle,
                                            const PcmBufferView& pcm,
                                            std::size_t packetCount = 128);
    static AmdtpTransmitRing createTone44100(FireWireDevice& device,
                                             UInt32 firstCycle,
                                             std::size_t pcmPosition,
                                             double frequencyHz = 1000.0,
                                             double amplitude = 131072.0,
                                             std::size_t packetCount = 128);

    RefillResult refillPcm48k(PcmRingBuffer& pcm,
                              std::size_t firstPacket,
                              std::size_t packetCount);

    // Rebuild complete native-44.1 packets for a recycled range. Unlike the
    // 48-kHz path, DBC/SYT cannot be allowed to repeat at a short ring wrap.
    // The caller owns the continuation state and cycle cursor. A 640-packet
    // ring is recommended because the 441/640 data/NODATA pattern repeats at
    // exactly that boundary, keeping each DCL slot's packet length stable.
    RefillResult refillPcm44100(PcmRingBuffer& pcm,
                                std::size_t firstPacket,
                                std::size_t packetCount,
                                am824::Playback44100State& state,
                                UInt32& cycle) {
        RefillResult result{};
        if (!slots_ || packetCount_ == 0 || !pcm.valid() ||
            pcm.channelCount() != am824::kPlayback44100PcmPositions ||
            firstPacket >= packetCount_ || packetCount == 0)
            return result;

        const std::size_t end = std::min(packetCount_, firstPacket + packetCount);
        std::int32_t frames[am824::kPlayback44100EventsPerDataPacket *
                            am824::kPlayback44100PcmPositions]{};

        for (std::size_t i = firstPacket; i < end; ++i) {
            ++result.packetsVisited;
            auto packet = am824::buildPlayback44100Silence(cycle & 0x1fffu, state);
            if (packet.length != slots_[i].length ||
                packet.dataBearing != slots_[i].dataBearing)
                return RefillResult{};

            if (packet.dataBearing) {
                const auto rr = pcm.read(frames, am824::kPlayback44100EventsPerDataPacket);
                result.framesRequested += rr.framesRequested;
                result.framesFromBuffer += rr.framesFromBuffer;
                result.framesSilenced += rr.framesSilenced;
                for (std::size_t event = 0; event < am824::kPlayback44100EventsPerDataPacket; ++event) {
                    for (std::size_t channel = 0; channel < am824::kPlayback44100PcmPositions; ++channel) {
                        const auto sample = std::max<std::int32_t>(-8388608,
                            std::min<std::int32_t>(8388607,
                                frames[event * am824::kPlayback44100PcmPositions + channel]));
                        const std::uint32_t mbla = 0x40000000u |
                            (static_cast<std::uint32_t>(sample) & 0x00ffffffu);
                        const std::size_t off = 8 +
                            (event * am824::kPlayback44100Positions + channel) * 4;
                        am824::putBe32Playback(packet.bytes.data() + off, mbla);
                    }
                }
                ++result.dataPacketsRefilled;
            }

            auto* dst = const_cast<std::uint8_t*>(slots_[i].payload);
            std::copy_n(packet.bytes.data(), packet.length, dst);
            slots_[i].cycle = cycle & 0x1fffu;
            slots_[i].dbc = packet.dbc;
            slots_[i].syt = packet.syt;
            ++cycle;
        }

        std::atomic_thread_fence(std::memory_order_release);
        return result;
    }

    explicit operator bool() const { return localPort_ != nullptr; }
    IOFireWireLibLocalIsochPortRef nativeLocalPort() const { return localPort_; }
    UInt32 firstCycle() const { return firstCycle_; }
    std::size_t packetCount() const { return packetCount_; }
    const PacketSlot& slot(std::size_t index) const;

private:
    struct StorageSlot;
    static AmdtpTransmitRing create48k(FireWireDevice& device,
                                       UInt32 firstCycle,
                                       const PcmBufferView* pcm,
                                       std::size_t packetCount);
    static AmdtpTransmitRing create44100(FireWireDevice& device,
                                         UInt32 firstCycle,
                                         const PcmBufferView* pcm,
                                         std::size_t packetCount);
    void reset();
    void moveFrom(AmdtpTransmitRing&& other) noexcept;

    FireWireDevice* device_ = nullptr;
    StorageSlot* storage_ = nullptr;
    PacketSlot* slots_ = nullptr;
    std::size_t packetCount_ = 0;
    std::size_t mappedBytes_ = 0;
    UInt32 firstCycle_ = 0;
    IOFireWireLibNuDCLPoolRef pool_ = nullptr;
    IOFireWireLibLocalIsochPortRef localPort_ = nullptr;
};
} // namespace macfw
