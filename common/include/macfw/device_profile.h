#pragma once

#include <cstddef>
#include <cstdint>

namespace macfw::devices {

enum class Personality : std::uint8_t {
    Operational,
    Bootloader,
};

enum class SupportLevel : std::uint8_t {
    Experimental,
    Supported,
};

struct IdentityMatch {
    const char* productName;
    Personality personality;
    std::uint32_t unitSpecifierId;
    std::uint32_t unitSwVersion;
    bool requireSwVersion;
};

struct DeviceProfile {
    const char* id;
    const char* vendor;
    const char* model;
    SupportLevel supportLevel;
    const IdentityMatch* identities;
    std::size_t identityCount;
    const std::uint32_t* initialSampleRates;
    std::size_t initialSampleRateCount;
};

inline constexpr const char* personalityName(Personality p) {
    return p == Personality::Operational ? "operational" : "bootloader";
}

inline constexpr const char* supportLevelName(SupportLevel s) {
    return s == SupportLevel::Supported ? "supported" : "experimental";
}

} // namespace macfw::devices
