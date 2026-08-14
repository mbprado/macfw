#include "macfw/isoch_allocation.h"
#include <utility>

namespace macfw {

IsochAllocation::~IsochAllocation() { reset(); }

IsochAllocation::IsochAllocation(IsochAllocation&& other) noexcept {
    moveFrom(std::move(other));
}

IsochAllocation& IsochAllocation::operator=(IsochAllocation&& other) noexcept {
    if (this != &other) {
        reset();
        moveFrom(std::move(other));
    }
    return *this;
}

void IsochAllocation::moveFrom(IsochAllocation&& other) noexcept {
    device_ = other.device_;
    direction_ = other.direction_;
    maxPayloadBytes_ = other.maxPayloadBytes_;
    channel_ = other.channel_;
    remotePort_ = other.remotePort_;
    allocated_ = other.allocated_;
    speed_ = other.speed_;
    channelNumber_ = other.channelNumber_;

    portState_.owner = this;

    if (remotePort_) {
        auto basePort =
            reinterpret_cast<IOFireWireLibIsochPortRef>(remotePort_);
        (*basePort)->SetRefCon(basePort, &portState_);
    }

    other.device_ = nullptr;
    other.maxPayloadBytes_ = 0;
    other.channel_ = nullptr;
    other.remotePort_ = nullptr;
    other.portState_.owner = nullptr;
    other.allocated_ = false;
    other.speed_ = kFWSpeed100MBit;
    other.channelNumber_ = 0;
}

IOReturn IsochAllocation::getSupported(IOFireWireLibIsochPortRef,
                                       IOFWSpeed* outMaxSpeed,
                                       UInt64* outChannels) {
    if (outMaxSpeed) *outMaxSpeed = kFWSpeed400MBit;
    if (outChannels) *outChannels = ~static_cast<UInt64>(0);
    return kIOReturnSuccess;
}

IOReturn IsochAllocation::onAllocate(IOFireWireLibIsochPortRef interface,
                                     IOFWSpeed speed,
                                     UInt32 channel) {
    auto* state =
        static_cast<PortState*>((*interface)->GetRefCon(interface));

    if (state && state->owner) {
        state->owner->allocated_ = true;
        state->owner->speed_ = speed;
        state->owner->channelNumber_ = channel;
    }
    return kIOReturnSuccess;
}

IOReturn IsochAllocation::onRelease(IOFireWireLibIsochPortRef interface) {
    auto* state =
        static_cast<PortState*>((*interface)->GetRefCon(interface));
    if (state && state->owner)
        state->owner->allocated_ = false;
    return kIOReturnSuccess;
}

IOReturn IsochAllocation::noop(IOFireWireLibIsochPortRef) {
    return kIOReturnSuccess;
}

IsochAllocation IsochAllocation::create(FireWireDevice& device,
                                        Direction direction,
                                        UInt32 maxPayloadBytes) {
    IsochAllocation result;
    if (!device.nativeHandle())
        return result;

    result.device_ = &device;
    result.direction_ = direction;
    result.maxPayloadBytes_ = maxPayloadBytes;
    result.portState_.owner = &result;

    auto native = device.nativeHandle();

    result.channel_ = (*native)->CreateIsochChannel(
        native, true, maxPayloadBytes, kFWSpeed400MBit,
        CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));

    if (!result.channel_) {
        result.reset();
        return result;
    }

    const Boolean remoteIsTalker =
        direction == Direction::DeviceToHost ? true : false;

    result.remotePort_ = (*native)->CreateRemoteIsochPort(
        native, remoteIsTalker,
        CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));

    if (!result.remotePort_) {
        result.reset();
        return result;
    }

    auto basePort =
        reinterpret_cast<IOFireWireLibIsochPortRef>(result.remotePort_);
    (*basePort)->SetRefCon(basePort, &result.portState_);
    (*result.remotePort_)->SetGetSupportedHandler(
        result.remotePort_, getSupported);
    (*result.remotePort_)->SetAllocatePortHandler(
        result.remotePort_, onAllocate);
    (*result.remotePort_)->SetReleasePortHandler(
        result.remotePort_, onRelease);
    (*result.remotePort_)->SetStartHandler(
        result.remotePort_, noop);
    (*result.remotePort_)->SetStopHandler(
        result.remotePort_, noop);

    if (direction == Direction::DeviceToHost) {
        (*result.channel_)->SetTalker(result.channel_, basePort);
    }
    // HostToDevice listener attachment is intentionally deferred.
    // The proven FW410 path attaches the local talker first.

    return result;
}

IOReturn IsochAllocation::bindHostToDeviceTalkerFirst(
    IOFireWireLibLocalIsochPortRef localTalker) {

    if (direction_ != Direction::HostToDevice ||
        !channel_ || !remotePort_ || !localTalker)
        return kIOReturnBadArgument;

    auto localBase =
        reinterpret_cast<IOFireWireLibIsochPortRef>(localTalker);
    auto remoteBase =
        reinterpret_cast<IOFireWireLibIsochPortRef>(remotePort_);

    IOReturn kr = (*channel_)->SetTalker(channel_, localBase);
    if (kr != kIOReturnSuccess)
        return kr;

    return (*channel_)->AddListener(channel_, remoteBase);
}

IOReturn IsochAllocation::allocate() {
    if (!channel_)
        return kIOReturnNoDevice;
    if (allocated_)
        return kIOReturnSuccess;

    const IOReturn kr = (*channel_)->AllocateChannel(channel_);
    if (kr != kIOReturnSuccess)
        return kr;

    return allocated_ ? kIOReturnSuccess : kIOReturnError;
}

IOReturn IsochAllocation::release() {
    if (!channel_ || !allocated_)
        return kIOReturnSuccess;

    const IOReturn kr = (*channel_)->ReleaseChannel(channel_);
    if (kr == kIOReturnSuccess)
        allocated_ = false;
    return kr;
}

void IsochAllocation::reset() {
    release();

    if (remotePort_) {
        (*remotePort_)->Release(remotePort_);
        remotePort_ = nullptr;
    }

    if (channel_) {
        (*channel_)->Release(channel_);
        channel_ = nullptr;
    }

    device_ = nullptr;
    portState_.owner = nullptr;
    allocated_ = false;
    speed_ = kFWSpeed100MBit;
    channelNumber_ = 0;
    maxPayloadBytes_ = 0;
}

} // namespace macfw
