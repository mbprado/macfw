#include "macfw/am824_playback.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

constexpr std::size_t kRing = 640;
constexpr std::size_t kExtendedRing = 1280;
constexpr std::size_t kEvents = 32;
constexpr std::size_t kPlaybackDbs = 5; // Four PCM positions and one MIDI.
constexpr std::size_t kCaptureDbs = 3;  // Two PCM positions and one MIDI.

bool check176400() {
    // At 176.4 kHz a 32-event block has the same 44.1-family SYT period as
    // the validated 88.2-kHz 16-event block. The per-packet DBC step and
    // payload sizes are different; these checks keep that distinction visible.
    constexpr std::size_t kPeriod = 10240;
    constexpr std::size_t kPlaybackBytes = 8 + kEvents * kPlaybackDbs * 4;
    constexpr std::size_t kCaptureBytes = 8 + kEvents * kCaptureDbs * 4;
    static_assert(kPlaybackBytes == 648 && kCaptureBytes == 392);
    std::array<bool, kRing> firstLengths{};
    macfw::am824::Playback44100State state{};
    std::size_t dataCount = 0;
    for (std::size_t cycle = 0; cycle < kPeriod; ++cycle) {
        const bool data = macfw::am824::nextPlayback44100SytOffset(state) <
            macfw::am824::kTicksPerCycle;
        if (cycle < kRing) firstLengths[cycle] = data;
        else if (data != firstLengths[cycle % kRing]) return false;
        if (data) {
            ++dataCount;
            state.dbc = static_cast<std::uint8_t>(state.dbc + kEvents);
        }
        if (cycle == kRing - 1 &&
            (dataCount != 441 || state.dbc != 32)) return false;
        if (cycle == kExtendedRing - 1 &&
            (dataCount != 882 || state.dbc != 64)) return false;
    }
    return dataCount == 7056 && state.dbc == 0 &&
           state.lastSytOffset == macfw::am824::kTicksPerCycle &&
           state.sytOffsetPhase == 67;
}

bool check192000() {
    // 192 kHz / 32 events yields exactly three data packets and one NODATA
    // packet per four cycles. Each repeated NuDCL slot retains its length.
    constexpr std::size_t kPeriod = 8000;
    std::size_t dataCount = 0;
    std::uint8_t dbc = 0;
    for (std::size_t cycle = 0; cycle < kPeriod; ++cycle) {
        const bool data = cycle % 4 != 3;
        if (data) {
            ++dataCount;
            dbc = static_cast<std::uint8_t>(dbc + kEvents);
        }
        if (cycle == kRing - 1 && (dataCount != 480 || dbc != 0))
            return false;
        if (cycle == kExtendedRing - 1 && (dataCount != 960 || dbc != 0))
            return false;
    }
    return dataCount == 6000 && dbc == 0;
}

} // namespace

int main() {
    if (!check176400() || !check192000()) return 1;
    std::printf("quad-rate schedule: PASS (176.4: 441/640 data, 32 events; "
                "192: 3/4 data, 32 events; TX max 648, RX max 392 bytes)\n");
    return 0;
}
