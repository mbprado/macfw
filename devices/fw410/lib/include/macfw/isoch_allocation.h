#pragma once

#include "macfw/firewire_device.h"
#include <IOKit/firewire/IOFireWireLibIsoch.h>

namespace macfw {

class IsochAllocation {
public:
    enum class Direction { DeviceToHost, HostToDevice };

    IsochAllocation() = default;
    ~IsochAllocation();

    IsochAllocation(const IsochAllocation&) = delete;
    IsochAllocation& operator=(const IsochAllocation&) = delete;

    IsochAllocation(IsochAllocation&& other) noexcept;
    IsochAllocation& operator=(IsochAllocation&& other) noexcept;

    static IsochAllocation create(FireWireDevice& device,
                                  Direction direction,
                                  UInt32 maxPayloadBytes);

    explicit operator bool() const {
        return channel_ != nullptr && remotePort_ != nullptr;
    }

    IOReturn allocate();
    IOReturn release();

    bool allocated() const { return allocated_; }
    UInt32 channel() const { return channelNumber_; }
    IOFWSpeed speed() const { return speed_; }

    IOFireWireLibIsochChannelRef nativeChannel() const {
        return channel_;
    }

    IOFireWireLibRemoteIsochPortRef nativeRemotePort() const {
        return remotePort_;
    }

    IOReturn bindHostToDeviceTalkerFirst(
        IOFireWireLibLocalIsochPortRef localTalker);

private:
    struct PortState { IsochAllocation* owner = nullptr; };

    static IOReturn getSupported(IOFireWireLibIsochPortRef,
                                 IOFWSpeed* outMaxSpeed,
                                 UInt64* outChannels);
    static IOReturn onAllocate(IOFireWireLibIsochPortRef,
                               IOFWSpeed speed,
                               UInt32 channel);
    static IOReturn onRelease(IOFireWireLibIsochPortRef);
    static IOReturn noop(IOFireWireLibIsochPortRef);

    void reset();
    void moveFrom(IsochAllocation&& other) noexcept;

    FireWireDevice* device_ = nullptr;
    Direction direction_ = Direction::DeviceToHost;
    UInt32 maxPayloadBytes_ = 0;
    IOFireWireLibIsochChannelRef channel_ = nullptr;
    IOFireWireLibRemoteIsochPortRef remotePort_ = nullptr;
    PortState portState_{};
    bool allocated_ = false;
    IOFWSpeed speed_ = kFWSpeed100MBit;
    UInt32 channelNumber_ = 0;
};

} // namespace macfw
