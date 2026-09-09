#pragma once

#include "macfw/firewire_device.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace macfw::fw1814 {

// FFADO-documented M-Audio special-firmware mixer area. These registers are
// write-only on FW1814/ProjectMix; never read them back or probe nearby offsets.
inline constexpr UInt16 kMixerAddressHi = 0xffc7;
inline constexpr UInt32 kMixStreamInLo = 0x00700094;  // MIX_STM_IN
inline constexpr UInt32 kSrcAnalogOutLo = 0x0070009c; // SRC_ANA_OUT

inline constexpr std::uint32_t kStraightStreamToMixer = 0x00000006u;
inline constexpr std::uint32_t kAnalogFromMixers = 0x00000000u;

inline std::array<std::uint8_t, 4> mixerBe32(std::uint32_t value) {
    return {{
        static_cast<std::uint8_t>((value >> 24) & 0xffu),
        static_cast<std::uint8_t>((value >> 16) & 0xffu),
        static_cast<std::uint8_t>((value >> 8) & 0xffu),
        static_cast<std::uint8_t>(value & 0xffu),
    }};
}

inline bool writeMixerRegister(FireWireDevice& device,
                               UInt32 addressLo,
                               std::uint32_t value) {
    const auto bytes = mixerBe32(value);
    UInt32 size = static_cast<UInt32>(bytes.size());
    const IOReturn kr = device.write(
        kMixerAddressHi, addressLo, bytes.data(), size);
    return kr == kIOReturnSuccess && size == bytes.size();
}

inline bool applyStraightAnalogPlaybackRouting(FireWireDevice& device,
                                               bool verbose = true) {
    auto native = device.nativeHandle();
    if (!native) return false;
    const UInt32 expectedGeneration = device.generation();

    if (!writeMixerRegister(device, kMixStreamInLo, kStraightStreamToMixer)) {
        if (verbose) std::cerr << "FW1814 MIX_STM_IN write failed\n";
        return false;
    }

    UInt32 generation = 0;
    if ((*native)->GetBusGeneration(native, &generation) != kIOReturnSuccess ||
        generation != expectedGeneration) {
        if (verbose) std::cerr << "FW1814 generation changed after MIX_STM_IN; stopping routing sequence\n";
        return false;
    }

    if (!writeMixerRegister(device, kSrcAnalogOutLo, kAnalogFromMixers)) {
        if (verbose) std::cerr << "FW1814 SRC_ANA_OUT write failed\n";
        return false;
    }

    if ((*native)->GetBusGeneration(native, &generation) != kIOReturnSuccess ||
        generation != expectedGeneration) {
        if (verbose) std::cerr << "FW1814 generation changed after SRC_ANA_OUT\n";
        return false;
    }

    if (verbose)
        std::cout << "FW1814 analog routing: Stream 1/2->Mix 1/2->Analog 1/2, "
                     "Stream 3/4->Mix 3/4->Analog 3/4\n";
    return true;
}

} // namespace macfw::fw1814
