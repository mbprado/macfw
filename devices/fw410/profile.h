#pragma once

#include "macfw/device_profile.h"

namespace macfw::devices::fw410 {

inline constexpr std::uint32_t kInitialSampleRates[] = {
    44100,
    48000,
};

inline constexpr IdentityMatch kIdentities[] = {
    {
        "FW 410",
        Personality::Operational,
        0x0000a02d,
        0,
        false,
    },
    {
        "FW Bootloader",
        Personality::Bootloader,
        0x0000a02d,
        0x00014001,
        true,
    },
};

inline constexpr DeviceProfile kProfile = {
    "fw410",
    "M-Audio",
    "FireWire 410",
    SupportLevel::Supported,
    kIdentities,
    sizeof(kIdentities) / sizeof(kIdentities[0]),
    kInitialSampleRates,
    sizeof(kInitialSampleRates) / sizeof(kInitialSampleRates[0]),
};

} // namespace macfw::devices::fw410
