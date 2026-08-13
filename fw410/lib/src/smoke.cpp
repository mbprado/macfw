#include "macfw/channel_map.h"
#include "macfw/am824.h"

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

    return 0;
}
