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

// Intentionally empty until the connected development FW1814 has been
// fingerprinted on macOS. Published Linux/BeBoB identifiers are useful
// reference evidence, but are not promoted into macfw installer/runtime
// matching before local hardware validation.
inline constexpr DeviceProfile kProfile = {
    "fw1814",
    "M-Audio",
    "FireWire 1814",
    SupportLevel::Experimental,
    nullptr,
    0,
    kInitialSampleRates,
    sizeof(kInitialSampleRates) / sizeof(kInitialSampleRates[0]),
};

} // namespace macfw::devices::fw1814
