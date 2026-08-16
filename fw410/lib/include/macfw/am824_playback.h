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
constexpr std::uint32_t kSytWrapTicks = 16u * kTicksPerCycle;
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

// 44.1 kHz uses 8-frame blocking packets, but only 441 of each 640
// FireWire cycles carry data: 44100 / 8000 / 8 = 441 / 640.
// packetAccumulator=441 reproduces the phase observed from the FW410
// (D,D,N,D,D,N,D,D,N,D,D,N,D,D,D,N,...), while remaining a rational
// scheduler rather than a hard-coded pattern.
struct Playback44100State {
    std::uint8_t dbc = 0;
    std::uint16_t packetAccumulator = 441;
    bool sytInitialized = false;
    std::uint32_t sytTicks = 0;
    std::uint32_t sytRemainder = 0;
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

inline std::uint16_t computePlayback48kSyt(std::uint32_t cycle,
                                           std::uint32_t sytOffsetTicks) {
    const std::uint32_t total =
        sytOffsetTicks + kPlayback48kTransferDelayTicks;
    const std::uint32_t presentationCycle =
        cycle + total / kTicksPerCycle;
    const std::uint32_t presentationOffset =
        total % kTicksPerCycle;
    return static_cast<std::uint16_t>(
        ((presentationCycle & 0x0fu) << 12) | presentationOffset);
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

inline std::uint16_t sytFromTicks(std::uint32_t ticks) {
    ticks %= kSytWrapTicks;
    const std::uint32_t cycle = ticks / kTicksPerCycle;
    const std::uint32_t offset = ticks % kTicksPerCycle;
    return static_cast<std::uint16_t>((cycle << 12) | offset);
}

inline void advancePlayback44100Syt(Playback44100State &state) {
    // Eight 44.1-kHz samples occupy 196608000 / 44100 FireWire ticks.
    // Integer part is 4458 ticks, remainder 10200/44100. Accumulating the
    // remainder yields the observed 4458/4459-tick SYT increments.
    std::uint32_t delta = 4458u;
    state.sytRemainder += 10200u;
    if (state.sytRemainder >= 44100u) {
        state.sytRemainder -= 44100u;
        ++delta;
    }
    state.sytTicks = (state.sytTicks + delta) % kSytWrapTicks;
}

inline Playback44100Packet buildPlayback44100Silence(
    std::uint32_t txCycle, Playback44100State &state) {
    Playback44100Packet packet;
    packet.dbc = state.dbc;

    state.packetAccumulator = static_cast<std::uint16_t>(
        state.packetAccumulator + 441u);
    if (state.packetAccumulator >= 640u) {
        state.packetAccumulator = static_cast<std::uint16_t>(
            state.packetAccumulator - 640u);
        packet.dataBearing = true;
    }

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

    if (!state.sytInitialized) {
        state.sytTicks =
            ((txCycle & 0x0fu) * kTicksPerCycle +
             kPlayback44100TransferDelayTicks) % kSytWrapTicks;
        state.sytInitialized = true;
    } else {
        advancePlayback44100Syt(state);
    }

    packet.syt = sytFromTicks(state.sytTicks);
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
