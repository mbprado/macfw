#pragma once
#include "macfw/am824_playback.h"
#include "macfw/firewire_device.h"
#include "macfw/pcm_buffer.h"
#include "macfw/pcm_ring_buffer.h"
#include <IOKit/firewire/IOFireWireLibIsoch.h>
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

    // Packetize interleaved PCM into the FW410's 10 playback PCM positions.
    // Missing channels/frames are zero-filled. Samples outside signed 24-bit
    // range are clipped. The source is consumed during construction only.
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

    // Replace only PCM payload words in already-built send slots. CIP timing,
    // DBC/SYT and DCL structure are left untouched. This is intentionally a
    // data-buffer update, not a running-DCL metadata update. Callers must only
    // refill slots known not to be in the DMA consumer's active window.
    // The PCM ring must expose exactly the FW410's 10 playback positions.
    RefillResult refillPcm48k(PcmRingBuffer& pcm,
                              std::size_t firstPacket,
                              std::size_t packetCount);

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
