#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace macfw::am824 {

constexpr std::size_t kPlayback48kPositions = 11;
constexpr std::size_t kPlayback48kPcmPositions = 10;
constexpr std::size_t kPlayback48kEventsPerDataPacket = 8;
constexpr std::size_t kPlayback48kDataPacketBytes =
    8 + kPlayback48kEventsPerDataPacket * kPlayback48kPositions * 4;

constexpr std::size_t kPlayback44100Positions = 11;
constexpr std::size_t kPlayback44100PcmPositions = 10;
constexpr std::size_t kPlayback44100EventsPerDataPacket = 8;
constexpr std::size_t kPlayback44100DataPacketBytes =
    8 + kPlayback44100EventsPerDataPacket * kPlayback44100Positions * 4;

constexpr std::uint32_t kMblaSilence = 0x40000000u;
constexpr std::uint32_t kMidiNoData = 0x80000000u;

constexpr std::uint32_t kTicksPerCycle = 3072u;
constexpr std::uint32_t kTicksPerSecond = 24576000u;
constexpr std::uint32_t kPlayback48kTransferDelayTicks =
    0x2e00u - kTicksPerCycle + (kTicksPerSecond * 8u / 48000u);
constexpr std::uint32_t kPlayback44100TransferDelayTicks =
    0x2e00u - kTicksPerCycle + (kTicksPerSecond * 8u / 44100u);

struct Playback48kState {
    std::uint8_t dbc = 0;
    std::uint8_t phase = 0;
};

struct Playback48kPacket {
    std::array<std::uint8_t, kPlayback48kDataPacketBytes> bytes{};
    std::uint32_t length = 0;
    bool dataBearing = false;
    std::uint8_t dbc = 0;
    std::uint16_t syt = 0xffffu;
};

// Native 44.1-kHz blocking AMDTP state. The initial values mirror the
// blocking base-44.1 state used by Linux's AMDTP implementation:
// last_syt_offset = TICKS_PER_CYCLE and syt_offset_state = 67.
// Starting at phase zero has the correct long-term average but shifts the
// whole data/NODATA and SYT sequence relative to the 1394 bus cycles.
struct Playback44100State {
    std::uint8_t dbc = 0;
    std::uint32_t lastSytOffset = kTicksPerCycle;
    std::uint16_t sytOffsetPhase = 67;
};

struct Playback44100Packet {
    std::array<std::uint8_t, kPlayback44100DataPacketBytes> bytes{};
    std::uint32_t length = 0;
    bool dataBearing = false;
    std::uint8_t dbc = 0;
    std::uint16_t syt = 0xffffu;
};

inline void putBe32Playback(std::uint8_t *p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
    p[1] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
    p[2] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    p[3] = static_cast<std::uint8_t>(value & 0xffu);
}

inline std::uint16_t computePlaybackSyt(std::uint32_t cycle,
                                        std::uint32_t sytOffsetTicks,
                                        std::uint32_t transferDelayTicks) {
    const std::uint32_t total = sytOffsetTicks + transferDelayTicks;
    const std::uint32_t presentationCycle = cycle + total / kTicksPerCycle;
    const std::uint32_t presentationOffset = total % kTicksPerCycle;
    return static_cast<std::uint16_t>(
        ((presentationCycle & 0x0fu) << 12) | presentationOffset);
}

inline std::uint16_t computePlayback48kSyt(std::uint32_t cycle,
                                           std::uint32_t sytOffsetTicks) {
    return computePlaybackSyt(cycle, sytOffsetTicks,
                              kPlayback48kTransferDelayTicks);
}

inline Playback48kPacket buildPlayback48kSilence(
    std::uint32_t txCycle, Playback48kState &state) {
    Playback48kPacket packet;
    packet.dbc = state.dbc;
    packet.dataBearing = state.phase != 3u;

    putBe32Playback(
        packet.bytes.data(),
        (static_cast<std::uint32_t>(kPlayback48kPositions) << 16) |
            state.dbc);

    if (!packet.dataBearing) {
        putBe32Playback(packet.bytes.data() + 4, 0x9002ffffu);
        packet.length = 8;
        packet.syt = 0xffffu;
    } else {
        const std::uint32_t sytOffset =
            static_cast<std::uint32_t>(state.phase) * 1024u;
        packet.syt = computePlayback48kSyt(txCycle, sytOffset);
        putBe32Playback(
            packet.bytes.data() + 4,
            0x90020000u | static_cast<std::uint32_t>(packet.syt));

        std::size_t offset = 8;
        for (std::size_t event = 0;
             event < kPlayback48kEventsPerDataPacket; ++event) {
            for (std::size_t ch = 0;
                 ch < kPlayback48kPcmPositions; ++ch) {
                putBe32Playback(packet.bytes.data() + offset, kMblaSilence);
                offset += 4;
            }
            putBe32Playback(packet.bytes.data() + offset, kMidiNoData);
            offset += 4;
        }
        packet.length = static_cast<std::uint32_t>(offset);
    }

    return packet;
}

inline std::uint32_t nextPlayback44100SytOffset(Playback44100State &state) {
    std::uint32_t sytOffset;

    if (state.lastSytOffset < kTicksPerCycle) {
        const std::uint32_t phase = state.sytOffsetPhase;
        const std::uint32_t index = phase % 13u;
        const bool addTick =
            ((index != 0u) && ((index & 3u) == 0u)) || phase == 146u;

        sytOffset = state.lastSytOffset + 1386u + (addTick ? 1u : 0u);
        state.sytOffsetPhase = static_cast<std::uint16_t>(
            (phase + 1u) >= 147u ? 0u : (phase + 1u));
    } else {
        sytOffset = state.lastSytOffset - kTicksPerCycle;
    }

    state.lastSytOffset = sytOffset;
    return sytOffset;
}

inline Playback44100Packet buildPlayback44100Silence(
    std::uint32_t txCycle, Playback44100State &state) {
    Playback44100Packet packet;
    packet.dbc = state.dbc;

    const std::uint32_t sytOffset = nextPlayback44100SytOffset(state);
    packet.dataBearing = sytOffset < kTicksPerCycle;

    putBe32Playback(
        packet.bytes.data(),
        (static_cast<std::uint32_t>(kPlayback44100Positions) << 16) |
            state.dbc);

    if (!packet.dataBearing) {
        putBe32Playback(packet.bytes.data() + 4, 0x9001ffffu);
        packet.length = 8;
        packet.syt = 0xffffu;
        return packet;
    }

    packet.syt = computePlaybackSyt(txCycle, sytOffset,
                                    kPlayback44100TransferDelayTicks);
    putBe32Playback(packet.bytes.data() + 4,
                    0x90010000u | static_cast<std::uint32_t>(packet.syt));

    std::size_t offset = 8;
    for (std::size_t event = 0;
         event < kPlayback44100EventsPerDataPacket; ++event) {
        for (std::size_t ch = 0;
             ch < kPlayback44100PcmPositions; ++ch) {
            putBe32Playback(packet.bytes.data() + offset, kMblaSilence);
            offset += 4;
        }
        putBe32Playback(packet.bytes.data() + offset, kMidiNoData);
        offset += 4;
    }
    packet.length = static_cast<std::uint32_t>(offset);
    state.dbc = static_cast<std::uint8_t>(state.dbc + 8u);
    return packet;
}

} // namespace macfw::am824
