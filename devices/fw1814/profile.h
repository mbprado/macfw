#pragma once

#include "macfw/device_profile.h"

namespace macfw::devices::fw1814 {

// First macfw bring-up scope. The hardware is known to support higher rates,
// but they are deliberately deferred until the existing 44.1/48 kHz transport
// has been generalized and validated on real FW1814 hardware.
inline constexpr std::uint32_t kInitialSampleRates[] = {
    44100,
    48000,
};

// Confirmed on the local development FW1814 on macOS, 2026-09-06.
// Both personalities retain the same BeBoB unit specifier/SW-version pair;
// the product string distinguishes bootloader vs operational firmware.
inline constexpr IdentityMatch kIdentities[] = {
    {
        "FW 1814 Bootloader",
        Personality::Bootloader,
        0x0000a02d,
        0x00014001,
        true,
    },
    {
        "FW 1814",
        Personality::Operational,
        0x0000a02d,
        0x00014001,
        true,
    },
};

inline constexpr DeviceProfile kProfile = {
    "fw1814",
    "M-Audio",
    "FireWire 1814",
    SupportLevel::Experimental,
    kIdentities,
    sizeof(kIdentities) / sizeof(kIdentities[0]),
    kInitialSampleRates,
    sizeof(kInitialSampleRates) / sizeof(kInitialSampleRates[0]),
};

} // namespace macfw::devices::fw1814
