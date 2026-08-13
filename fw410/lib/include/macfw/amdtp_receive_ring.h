#pragma once

#include "macfw/amdtp_packet.h"
#include "macfw/firewire_device.h"
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <cstddef>
#include <cstdint>

namespace macfw {

class AmdtpReceiveRing {
public:
    struct PacketSlot {
        UInt32 isoHeader = 0;
        UInt32 status = 0;
        UInt32 timestamp = 0;
        std::uint8_t* payload = nullptr;
        std::size_t capacity = 0;

        std::size_t packetLength() const {
            return static_cast<std::size_t>(isoHeader >> 16);
        }

        amdtp::PacketView packet() const {
            const auto len = packetLength();
            return {payload, len <= capacity ? len : 0};
        }

        bool touched() const {
            return isoHeader != 0 || status != 0 || timestamp != 0;
        }
    };

    AmdtpReceiveRing() = default;
    ~AmdtpReceiveRing();
    AmdtpReceiveRing(const AmdtpReceiveRing&) = delete;
    AmdtpReceiveRing& operator=(const AmdtpReceiveRing&) = delete;
    AmdtpReceiveRing(AmdtpReceiveRing&& other) noexcept;
    AmdtpReceiveRing& operator=(AmdtpReceiveRing&& other) noexcept;

    static AmdtpReceiveRing create(FireWireDevice& device,
                                   std::size_t packetCount,
                                   std::size_t packetCapacity);

    explicit operator bool() const { return localPort_ != nullptr; }
    IOFireWireLibLocalIsochPortRef nativeLocalPort() const { return localPort_; }
    std::size_t packetCount() const { return packetCount_; }
    std::size_t packetCapacity() const { return packetCapacity_; }
    const PacketSlot& slot(std::size_t index) const;
    std::size_t touchedCount() const;
    bool completed() const { return completed_; }

private:
    struct RawSlot;
    struct CallbackState;

    static void onComplete(CallbackState* state, NuDCLRef);
    void reset();
    void moveFrom(AmdtpReceiveRing&& other) noexcept;
    void syncSlot(std::size_t index) const;

    FireWireDevice* device_ = nullptr;
    RawSlot* rawSlots_ = nullptr;
    std::uint8_t* payloadBase_ = nullptr;
    mutable PacketSlot* slots_ = nullptr;
    CallbackState* callbackState_ = nullptr;
    std::size_t packetCount_ = 0;
    std::size_t packetCapacity_ = 0;
    std::size_t metadataBytes_ = 0;
    std::size_t payloadBytes_ = 0;
    bool completed_ = false;
    IOFireWireLibNuDCLPoolRef pool_ = nullptr;
    IOFireWireLibLocalIsochPortRef localPort_ = nullptr;
};

} // namespace macfw
