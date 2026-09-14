#pragma once

#include "macfw/firewire_device.h"
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <cstddef>
#include <cstdint>

namespace macfw {
class AmdtpNoDataTransmitRing {
public:
    AmdtpNoDataTransmitRing() = default;
    ~AmdtpNoDataTransmitRing();
    AmdtpNoDataTransmitRing(const AmdtpNoDataTransmitRing&) = delete;
    AmdtpNoDataTransmitRing& operator=(const AmdtpNoDataTransmitRing&) = delete;
    AmdtpNoDataTransmitRing(AmdtpNoDataTransmitRing&& other) noexcept;
    AmdtpNoDataTransmitRing& operator=(AmdtpNoDataTransmitRing&& other) noexcept;

    static AmdtpNoDataTransmitRing create(FireWireDevice& device,
                                          UInt32 firstCycle,
                                          std::uint8_t fdf,
                                          std::uint8_t dbs = 11,
                                          std::size_t packetCount = 128);

    explicit operator bool() const { return localPort_ != nullptr; }
    IOFireWireLibLocalIsochPortRef nativeLocalPort() const { return localPort_; }
    UInt32 firstCycle() const { return firstCycle_; }
    std::size_t packetCount() const { return packetCount_; }

private:
    struct StorageSlot;
    void reset();
    void moveFrom(AmdtpNoDataTransmitRing&& other) noexcept;

    StorageSlot* storage_ = nullptr;
    std::size_t packetCount_ = 0;
    std::size_t mappedBytes_ = 0;
    UInt32 firstCycle_ = 0;
    IOFireWireLibNuDCLPoolRef pool_ = nullptr;
    IOFireWireLibLocalIsochPortRef localPort_ = nullptr;
};
}
