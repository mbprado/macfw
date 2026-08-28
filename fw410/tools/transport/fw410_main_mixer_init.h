#pragma once

#include "full_duplex_fcp_control.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace macfw::transport::duplex {

// Initialize the FW410 14x10 main mixer the same way as
// snd-firewire-ctl-services' MaudioNormalMixerCtlOperation does for FW410:
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
// Linux default: stream pair N feeds the matching mixer output pair N; all
// other cells are off. This is intentionally called before isochronous audio
// starts. Do not use it as a live mixer refresh operation.
inline bool initializeFw410MainMixerLikeLinux(Fw410FcpControl& fcp) {
    static constexpr std::uint8_t kDstBlock = 0x01;
    static constexpr std::array<std::uint8_t, 7> kSrcBlocks =
        {0x02, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00};
    static constexpr std::array<std::uint8_t, 7> kSrcChannels =
        {0x01, 0x01, 0x01, 0x01, 0x03, 0x05, 0x07};
    static constexpr std::array<std::uint8_t, 5> kDstChannels =
        {0x01, 0x03, 0x05, 0x07, 0x09};

    for (std::size_t dst = 0; dst < kDstChannels.size(); ++dst) {
        for (std::size_t src = 0; src < kSrcBlocks.size(); ++src) {
            const bool enabled = src == (2 + dst);
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

    std::cout << "FW410 main mixer initialized with Linux default routing (35 CONTROL writes)\n";
    return true;
}

} // namespace macfw::transport::duplex
