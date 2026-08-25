#pragma once

#include <cstdint>
#include <string_view>

namespace macfw::deviceprobe {

enum class Personality {
    Operational,
    Bootloader,
};

struct SupportedDevice {
    std::string_view macfwId;
    std::string_view family;
    std::string_view model;
    std::string_view productName;
    Personality personality;
    std::uint32_t unitSpecifierId;
    std::uint32_t unitSwVersion;
    bool requireSwVersion;
};

// Registry shared conceptually by diagnostics, installer compatibility checks,
// and future per-device runtime selection. Add future supported interfaces here
// rather than hard-coding them in installer scripts.
inline constexpr SupportedDevice kSupportedDevices[] = {
    {
        "fw410",
        "M-Audio",
        "FireWire 410",
        "FW 410",
        Personality::Operational,
        0x0000a02d,
        0,
        false,
    },
    {
        "fw410",
        "M-Audio",
        "FireWire 410",
        "FW Bootloader",
        Personality::Bootloader,
        0x0000a02d,
        0x00014001,
        true,
    },
};

inline constexpr const char* personalityName(Personality p) {
    return p == Personality::Operational ? "operational" : "bootloader";
}

} // namespace macfw::deviceprobe
