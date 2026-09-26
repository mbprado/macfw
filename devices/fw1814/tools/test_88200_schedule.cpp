#include "macfw/am824_playback.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

int main() {
    // NuDCL send lengths cannot change when a half-ring is reused. Check the
    // actual 44.1-family state across the full DBC/SYT cycle, not just a
    // single 640-packet ring.
    constexpr std::size_t kRing = 640;
    constexpr std::size_t kExtendedRing = 1280;
    constexpr std::size_t kPeriod = 10240;
    std::array<bool, kRing> firstLengths{};
    macfw::am824::Playback44100State state{};
    std::size_t dataCount = 0;
    for (std::size_t cycle = 0; cycle < kPeriod; ++cycle) {
        const bool data = macfw::am824::nextPlayback44100SytOffset(state) <
            macfw::am824::kTicksPerCycle;
        if (cycle < kRing) firstLengths[cycle] = data;
        else if (data != firstLengths[cycle % kRing]) return 1;
        if (data) {
            ++dataCount;
            state.dbc = static_cast<std::uint8_t>(state.dbc + 16);
        }
        if (cycle == kRing - 1 &&
            (dataCount != 441 || state.dbc != 144 ||
             state.lastSytOffset != macfw::am824::kTicksPerCycle ||
             state.sytOffsetPhase != 67)) return 2;
        if (cycle == kExtendedRing - 1 &&
            (dataCount != 882 || state.dbc != 32 ||
             state.lastSytOffset != macfw::am824::kTicksPerCycle ||
             state.sytOffsetPhase != 67)) return 4;
    }
    if (dataCount != 7056 || state.dbc != 0 ||
        state.lastSytOffset != macfw::am824::kTicksPerCycle ||
        state.sytOffsetPhase != 67) return 3;
    std::printf("88.2 schedule: PASS (640/1280 packet ring lengths; "
                "441/882 data packets; 7056 data/full phase)\n");
    return 0;
}
