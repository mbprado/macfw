#include "macfw/channel_map.h"
#include "macfw/am824.h"
#include "macfw/amdtp_packet.h"

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

    return 0;
}
