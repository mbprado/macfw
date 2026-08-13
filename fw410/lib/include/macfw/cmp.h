#pragma once

#include "macfw/firewire_device.h"

#include <cstdint>

namespace macfw::cmp {

constexpr UInt16 kCsrAddressHi = 0xffff;
constexpr UInt32 kOpcr0AddressLo = 0xf0000904;
constexpr UInt32 kIpcr0AddressLo = 0xf0000984;

constexpr std::uint32_t kPcrOnline = 0x80000000u;
constexpr std::uint32_t kPcrBroadcast = 0x40000000u;
constexpr std::uint32_t kPcrP2PMask = 0x3f000000u;
constexpr std::uint32_t kPcrChannelMask = 0x003f0000u;
constexpr std::uint32_t kOpcrXSpeedMask = 0x00c00000u;
constexpr std::uint32_t kOpcrSpeedMask = 0x0000c000u;
constexpr std::uint32_t kOpcrOverheadMask = 0x00003c00u;

struct PcrState {
    std::uint32_t raw = 0;
    bool online = false;
    bool broadcast = false;
    std::uint8_t p2pConnections = 0;
    std::uint8_t channel = 0;
};

PcrState decodePcr(std::uint32_t value);
bool ready(const PcrState& state);

std::uint32_t makeConnectedIpcr(std::uint32_t original, UInt32 channel);
std::uint32_t makeConnectedOpcr(std::uint32_t original,
                                UInt32 channel,
                                IOFWSpeed speed);

IOReturn readOpcr0(const macfw::FireWireDevice& device, std::uint32_t& value);
IOReturn readIpcr0(const macfw::FireWireDevice& device, std::uint32_t& value);
IOReturn connectOpcr0(const macfw::FireWireDevice& device,
                      std::uint32_t expected,
                      UInt32 channel,
                      IOFWSpeed speed);
IOReturn connectIpcr0(const macfw::FireWireDevice& device,
                      std::uint32_t expected,
                      UInt32 channel);
IOReturn restore(const macfw::FireWireDevice& device,
                 UInt32 addressLo,
                 std::uint32_t original);

} // namespace macfw::cmp
