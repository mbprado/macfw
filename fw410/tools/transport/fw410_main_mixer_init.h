#pragma once

#include "full_duplex_fcp_control.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace macfw::transport::duplex {

// Initialize the FW410 14x10 main mixer using the same strategy as
// snd-firewire-ctl-services' MaudioNormalMixerCtlOperation for FW410:
// start from a known software-side matrix and issue CONTROL writes for every
// cell instead of relying on AV/C STATUS reads. The DM1000 ASIC is documented
// upstream as unreliable/heavy under mixer STATUS polling.
//
// Source indices:
//   0 analog in 1/2
//   1 S/PDIF in L/R
//   2 stream/software return 1/2
//   3 stream/software return 3/4
//   4 stream/software return 5/6
//   5 stream/software return 7/8
//   6 stream/software return 9/10
// Destination indices:
//   0..4 mixer out 1/2 .. 9/10
//
// Linux/ALSA uses stream-order identity routing. macfw exposes CoreAudio in
// physical-output order, while the FW410's 10 AMDTP PCM positions are
// interleaved as S1,A1,A3,A5,A7,S2,A2,A4,A6,A8. Therefore macfw's default
// routing must translate its physical channel order back to FW410 stream pairs:
//   mixer 1/2  <- stream 3/4   (CoreAudio Analog 1/2)
//   mixer 3/4  <- stream 5/6   (CoreAudio Analog 3/4)
//   mixer 5/6  <- stream 7/8   (CoreAudio Analog 5/6)
//   mixer 7/8  <- stream 9/10  (CoreAudio Analog 7/8)
//   mixer 9/10 <- stream 1/2   (CoreAudio S/PDIF L/R)
//
// This is intentionally called before isochronous audio starts. Do not use it
// as a live mixer refresh operation.
inline bool initializeFw410MainMixerLikeLinux(Fw410FcpControl& fcp) {
    static constexpr std::uint8_t kDstBlock = 0x01;
    static constexpr std::array<std::uint8_t, 7> kSrcBlocks =
        {0x02, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00};
    static constexpr std::array<std::uint8_t, 7> kSrcChannels =
        {0x01, 0x01, 0x01, 0x01, 0x03, 0x05, 0x07};
    static constexpr std::array<std::uint8_t, 5> kDstChannels =
        {0x01, 0x03, 0x05, 0x07, 0x09};
    static constexpr std::array<std::size_t, 5> kPlaybackSourceForDestination =
        {3, 4, 5, 6, 2};

    for (std::size_t dst = 0; dst < kDstChannels.size(); ++dst) {
        for (std::size_t src = 0; src < kSrcBlocks.size(); ++src) {
            const bool enabled = src == kPlaybackSourceForDestination[dst];
            if (!fcp.writeProcessingMixer(kDstBlock,
                                          kSrcBlocks[src],
                                          kSrcChannels[src],
                                          kDstChannels[dst],
                                          enabled)) {
                std::cerr << "FW410 main mixer initialization failed at src="
                          << src << " dst=" << dst << '\n';
                return false;
            }
        }
    }

    std::cout << "FW410 main mixer initialized with macfw playback routing "
                 "(35 Linux-style CONTROL writes)\n";
    return true;
}

} // namespace macfw::transport::duplex
