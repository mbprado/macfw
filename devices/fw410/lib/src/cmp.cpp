#include "macfw/cmp.h"

namespace macfw::cmp {

PcrState decodePcr(std::uint32_t value) {
    PcrState s;
    s.raw = value;
    s.online = (value & kPcrOnline) != 0;
    s.broadcast = (value & kPcrBroadcast) != 0;
    s.p2pConnections = static_cast<std::uint8_t>((value >> 24) & 0x3f);
    s.channel = static_cast<std::uint8_t>((value >> 16) & 0x3f);
    return s;
}

bool ready(const PcrState& state) {
    return state.online && !state.broadcast && state.p2pConnections == 0;
}

std::uint32_t makeConnectedIpcr(std::uint32_t original, UInt32 channel) {
    std::uint32_t v =
        original & ~(kPcrBroadcast | kPcrP2PMask | kPcrChannelMask);
    v |= (1u << 24);
    v |= (channel & 0x3f) << 16;
    return v;
}

std::uint32_t makeConnectedOpcr(std::uint32_t original,
                                UInt32 channel,
                                IOFWSpeed speed) {
    std::uint32_t v =
        original & ~(kPcrBroadcast | kPcrP2PMask | kPcrChannelMask |
                     kOpcrXSpeedMask | kOpcrSpeedMask | kOpcrOverheadMask);
    v |= (1u << 24);
    v |= (channel & 0x3f) << 16;
    const UInt32 speedCode = static_cast<UInt32>(speed) & 0x3;
    v |= speedCode << 14;
    return v;
}

IOReturn readOpcr0(const macfw::FireWireDevice& device,
                   std::uint32_t& value) {
    return device.readQuadletBE(kCsrAddressHi, kOpcr0AddressLo, value);
}

IOReturn readIpcr0(const macfw::FireWireDevice& device,
                   std::uint32_t& value) {
    return device.readQuadletBE(kCsrAddressHi, kIpcr0AddressLo, value);
}

IOReturn connectOpcr0(const macfw::FireWireDevice& device,
                      std::uint32_t expected,
                      UInt32 channel,
                      IOFWSpeed speed) {
    return device.compareSwapQuadletBE(
        kCsrAddressHi,
        kOpcr0AddressLo,
        expected,
        makeConnectedOpcr(expected, channel, speed));
}

IOReturn connectIpcr0(const macfw::FireWireDevice& device,
                      std::uint32_t expected,
                      UInt32 channel) {
    return device.compareSwapQuadletBE(
        kCsrAddressHi,
        kIpcr0AddressLo,
        expected,
        makeConnectedIpcr(expected, channel));
}

IOReturn restore(const macfw::FireWireDevice& device,
                 UInt32 addressLo,
                 std::uint32_t original) {
    std::uint32_t current = 0;
    IOReturn kr = device.readQuadletBE(kCsrAddressHi, addressLo, current);

    if (kr != kIOReturnSuccess || current == original)
        return kr;

    return device.compareSwapQuadletBE(
        kCsrAddressHi, addressLo, current, original);
}

} // namespace macfw::cmp
