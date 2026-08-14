#include "macfw/channel_map.h"
#include "macfw/am824.h"
#include "macfw/amdtp_packet.h"
#include "macfw/am824_playback.h"
#include "macfw/pcm_buffer.h"
#include "macfw/pcm_ring_buffer.h"

#include <cstdint>
#include <iostream>

int main() {
    const auto *capture = macfw::fw410::captureChannelForPosition(2);
    const auto *playback = macfw::fw410::playbackChannelForPosition(10);

    if (!capture || !playback)
        return 1;

    std::cout << "capture pos 2: " << capture->name << '\n';
    std::cout << "playback pos 10: " << playback->name << '\n';

    std::int32_t sample = 0;
    if (!macfw::am824::decodeMbla24(0x40000001u, sample) || sample != 1)
        return 2;

    const std::uint8_t capturePacket[8] = {
        0x00, 0x05, 0x00, 0x18,
        0x90, 0x02, 0xff, 0xff
    };

    const macfw::amdtp::PacketView packet{
        capturePacket, sizeof(capturePacket)
    };

    const auto cip = packet.cip();
    if (!packet.hasCip() ||
        !packet.isNoData() ||
        !macfw::amdtp::isFw410Capture48k(packet) ||
        cip.dbs != 5 ||
        cip.dbc != 0x18 ||
        cip.fmt != 0x10 ||
        cip.fdf != 0x02 ||
        cip.syt != 0xffff)
        return 3;

    std::cout << "AMDTP CIP parser: PASS\n";

    macfw::am824::Playback48kState txState{};
    const auto tx0 = macfw::am824::buildPlayback48kSilence(100, txState);
    if (!tx0.dataBearing || tx0.length != 360 || tx0.dbc != 0 || tx0.syt == 0xffff) return 4;
    txState.dbc = static_cast<std::uint8_t>(txState.dbc + 8u);
    txState.phase = 1;
    const auto tx1 = macfw::am824::buildPlayback48kSilence(101, txState);
    if (!tx1.dataBearing || tx1.length != 360 || tx1.dbc != 8 || tx1.syt == 0xffff) return 5;
    txState.dbc = static_cast<std::uint8_t>(txState.dbc + 8u);
    txState.phase = 2;
    const auto tx2 = macfw::am824::buildPlayback48kSilence(102, txState);
    if (!tx2.dataBearing || tx2.length != 360 || tx2.dbc != 16 || tx2.syt == 0xffff) return 6;
    txState.dbc = static_cast<std::uint8_t>(txState.dbc + 8u);
    txState.phase = 3;
    const auto tx3 = macfw::am824::buildPlayback48kSilence(103, txState);
    if (tx3.dataBearing || tx3.length != 8 || tx3.dbc != 24 || tx3.syt != 0xffff) return 7;
    std::cout << "AMDTP playback packet builder: PASS\n";

    const std::int32_t pcmSamples[] = {
        100, -100,
        200, -200,
    };
    const macfw::PcmBufferView pcm{pcmSamples, 2, 2, false};
    if (!pcm.valid() || pcm.sample(0, 0) != 100 || pcm.sample(1, 1) != -200 ||
        pcm.sample(2, 0) != 0 || pcm.sample(0, 2) != 0)
        return 8;

    const macfw::PcmBufferView looped{pcmSamples, 2, 2, true};
    if (looped.sample(2, 0) != 100 || looped.sample(3, 1) != -200)
        return 9;
    std::cout << "PCM buffer view: PASS\n";

    macfw::PcmRingBuffer ring(4, 2);
    if (!ring.valid() || ring.capacityFrames() != 4 || ring.channelCount() != 2)
        return 10;

    const std::int32_t firstWrite[] = {
        1, 101,
        2, 102,
        3, 103,
    };
    if (ring.write(firstWrite, 3) != 3 || ring.availableFrames() != 3 || ring.freeFrames() != 1)
        return 11;

    std::int32_t firstRead[4] = {};
    const auto r1 = ring.read(firstRead, 2);
    if (r1.framesFromBuffer != 2 || r1.framesSilenced != 0 ||
        firstRead[0] != 1 || firstRead[1] != 101 ||
        firstRead[2] != 2 || firstRead[3] != 102)
        return 12;

    const std::int32_t secondWrite[] = {
        4, 104,
        5, 105,
        6, 106,
    };
    if (ring.write(secondWrite, 3) != 3 || ring.availableFrames() != 4)
        return 13;

    std::int32_t wrapRead[10] = {};
    const auto r2 = ring.read(wrapRead, 5);
    if (r2.framesFromBuffer != 4 || r2.framesSilenced != 1 ||
        wrapRead[0] != 3 || wrapRead[1] != 103 ||
        wrapRead[2] != 4 || wrapRead[3] != 104 ||
        wrapRead[4] != 5 || wrapRead[5] != 105 ||
        wrapRead[6] != 6 || wrapRead[7] != 106 ||
        wrapRead[8] != 0 || wrapRead[9] != 0 ||
        ring.underrunFrames() != 1)
        return 14;

    if (ring.producedFrames() != 6 || ring.consumedFrames() != 6 || ring.availableFrames() != 0)
        return 15;

    ring.reset();
    if (ring.producedFrames() != 0 || ring.consumedFrames() != 0 || ring.underrunFrames() != 0)
        return 16;

    std::cout << "PCM ring buffer: PASS\n";

    return 0;
}
