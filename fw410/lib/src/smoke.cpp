#include "macfw/channel_map.h"
#include "macfw/am824.h"
#include "macfw/amdtp_packet.h"
#include "macfw/am824_playback.h"

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

    return 0;
}
