#include "macfw/firewire_device.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

constexpr const char* kProduct = "FW 1814";

// FFADO's M-Audio special-firmware implementation documents the mixer control
// area as:
//   MAUDIO_SPECIFIC_ADDRESS = 0xffc700000000
//   MAUDIO_CONTROL_OFFSET   = 0x00700000
// The FW1814 mixer registers are write-only, so this diagnostic intentionally
// performs no readback from this address range.
constexpr UInt16 kMixerAddressHi = 0xffc7;
constexpr UInt32 kMixStreamInLo   = 0x00700094; // MIX_STM_IN
constexpr UInt32 kSrcAnalogOutLo  = 0x0070009c; // SRC_ANA_OUT

// MIX_STM_IN low-nibble layout documented by FFADO:
//   bit 3: Stream 1/2 -> Mix 3/4
//   bit 2: Stream 1/2 -> Mix 1/2
//   bit 1: Stream 3/4 -> Mix 3/4
//   bit 0: Stream 3/4 -> Mix 1/2
// 0x06 therefore gives the straight stereo-pair routing we want.
constexpr std::uint32_t kStraightStreamToMixer = 0x00000006u;

// SRC_ANA_OUT bits clear select the corresponding internal mixer pair:
//   Analog 1/2 <- Mix 1/2
//   Analog 3/4 <- Mix 3/4
constexpr std::uint32_t kAnalogFromMixers = 0x00000000u;

std::array<std::uint8_t, 4> be32(std::uint32_t v) {
    return {{
        static_cast<std::uint8_t>((v >> 24) & 0xffu),
        static_cast<std::uint8_t>((v >> 16) & 0xffu),
        static_cast<std::uint8_t>((v >> 8) & 0xffu),
        static_cast<std::uint8_t>(v & 0xffu),
    }};
}

bool writeDocumentedRegister(macfw::FireWireDevice& device,
                             UInt32 addressLo,
                             std::uint32_t value,
                             const char* name) {
    const auto bytes = be32(value);
    UInt32 size = static_cast<UInt32>(bytes.size());
    const IOReturn kr = device.write(
        kMixerAddressHi, addressLo, bytes.data(), size);

    std::cout << "    " << name << " @ 0x"
              << std::hex << kMixerAddressHi << addressLo
              << " <- 0x" << value
              << ": IOReturn=0x" << kr
              << std::dec << ", bytes=" << size << '\n';

    if (kr != kIOReturnSuccess || size != bytes.size()) {
        std::cout << "    write result is not clean; stopping without further mixer writes\n";
        return false;
    }
    return true;
}

bool run(bool execute) {
    auto device = macfw::FireWireDevice::findByProductName(kProduct);
    if (!device) {
        std::cout << "No operational FW 1814 unit found.\n";
        return false;
    }

    if (device.open() != kIOReturnSuccess) {
        std::cout << "open failed\n";
        return false;
    }

    const UInt32 initialGeneration = device.generation();

    std::cout << "macfw FW1814 documented analog-stream routing diagnostic\n"
              << "    generation: " << initialGeneration << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID() << std::dec << '\n'
              << "    mixer control base: 0xffc700700000\n"
              << "    planned MIX_STM_IN  [0x94] = 0x00000006\n"
              << "        Stream 1/2 -> Mixer 1/2\n"
              << "        Stream 3/4 -> Mixer 3/4\n"
              << "    planned SRC_ANA_OUT [0x9c] = 0x00000000\n"
              << "        Analog 1/2 <- Mixer 1/2\n"
              << "        Analog 3/4 <- Mixer 3/4\n"
              << "    volume/pan/aux registers: UNTOUCHED\n"
              << "    AV/C/FCP commands: NONE\n"
              << "    ISO/CMP activity: NONE\n"
              << "    mixer readback: NONE (registers are write-only)\n";

    if (!execute) {
        std::cout << "status: PASS - dry run only; no FireWire writes made\n";
        device.close();
        return true;
    }

    if (!writeDocumentedRegister(
            device, kMixStreamInLo, kStraightStreamToMixer, "MIX_STM_IN")) {
        device.close();
        return false;
    }

    UInt32 generation = 0;
    auto native = device.nativeHandle();
    if (!native ||
        (*native)->GetBusGeneration(native, &generation) != kIOReturnSuccess ||
        generation != initialGeneration) {
        std::cout << "status: STOP - bus generation changed/unavailable after MIX_STM_IN; "
                     "SRC_ANA_OUT was not written\n";
        device.close();
        return false;
    }

    if (!writeDocumentedRegister(
            device, kSrcAnalogOutLo, kAnalogFromMixers, "SRC_ANA_OUT")) {
        device.close();
        return false;
    }

    if ((*native)->GetBusGeneration(native, &generation) != kIOReturnSuccess ||
        generation != initialGeneration) {
        std::cout << "status: STOP - bus generation changed/unavailable after SRC_ANA_OUT\n";
        device.close();
        return false;
    }

    std::cout << "status: PASS - documented straight analog playback routing written\n"
              << "note: mixer registers are write-only, so previous routing cannot be read/restored.\n";
    device.close();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") {
            execute = true;
        } else {
            std::cerr << "usage: " << argv[0] << " [--execute]\n";
            return 64;
        }
    }

    return run(execute) ? 0 : 1;
}
