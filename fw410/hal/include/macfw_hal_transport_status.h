#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace macfw::hal::transport {

// Keep the Darwin POSIX SHM object name comfortably below the platform's
// short name limit. The previous v1 name was 32 characters including '/'.
constexpr const char* kShmName = "/macfw_fw410_status_v1";
constexpr std::uint32_t kMagic = 0x4d465753; // 'MFWS'
constexpr std::uint32_t kVersion = 1;

enum class State : std::uint32_t {
    Offline = 0,
    Recovering = 1,
    Online = 2,
};

struct SharedStatus {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t structSize;
    std::uint32_t reserved0;

    std::atomic<std::uint32_t> state;
    std::atomic<std::uint32_t> requestedRate;
    std::atomic<std::uint32_t> activeRate;
    std::atomic<std::uint32_t> enginePid;
    std::atomic<std::uint64_t> transitionSequence;
    std::atomic<std::uint64_t> heartbeatSequence;
};

inline bool valid(const SharedStatus& status) {
    return status.magic == kMagic &&
           status.version == kVersion &&
           status.structSize == sizeof(SharedStatus);
}

inline const char* stateName(State state) {
    switch (state) {
        case State::Offline: return "OFFLINE";
        case State::Recovering: return "RECOVERING";
        case State::Online: return "ONLINE";
    }
    return "UNKNOWN";
}

} // namespace macfw::hal::transport
