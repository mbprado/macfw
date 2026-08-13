#include "macfw/firewire_device.h"

#include <cstdint>
#include <iomanip>
#include <iostream>

namespace {

constexpr UInt16 kAddressHi = 0xffff;
constexpr UInt32 kOmprLo = 0xf0000900;
constexpr UInt32 kOpcr0Lo = 0xf0000904;
constexpr UInt32 kImprLo = 0xf0000980;
constexpr UInt32 kIpcr0Lo = 0xf0000984;

static bool readReg(const macfw::FireWireDevice& device,
                    UInt32 lo, std::uint32_t& value) {
    const IOReturn kr = device.readQuadletBE(kAddressHi, lo, value);
    if (kr != kIOReturnSuccess) {
        std::cout << "        read failed at 0xffff" << std::hex << lo
                  << " (0x" << kr << ")" << std::dec << '\n';
        return false;
    }
    return true;
}

static void printMpr(const char* name, std::uint32_t v) {
    const unsigned speed = (v >> 30) & 0x3;
    const unsigned xspeed = (v >> 5) & 0x3;
    const unsigned plugs = v & 0x1f;
    std::cout << "    " << name << ": 0x" << std::hex << std::setw(8)
              << std::setfill('0') << v << std::dec << std::setfill(' ') << '\n';
    std::cout << "        base speed code: " << speed << '\n';
    std::cout << "        extended speed:  " << xspeed << '\n';
    std::cout << "        plug count:      " << plugs << '\n';
}

static void printPcr(const char* name, std::uint32_t v, bool output) {
    const bool online = (v & 0x80000000u) != 0;
    const bool broadcast = (v & 0x40000000u) != 0;
    const unsigned p2p = (v >> 24) & 0x3f;
    const unsigned channel = (v >> 16) & 0x3f;

    std::cout << "    " << name << ": 0x" << std::hex << std::setw(8)
              << std::setfill('0') << v << std::dec << std::setfill(' ') << '\n';
    std::cout << "        online:          " << (online ? "yes" : "no") << '\n';
    std::cout << "        broadcast conn:  " << (broadcast ? "yes" : "no") << '\n';
    std::cout << "        p2p connections: " << p2p << '\n';
    std::cout << "        channel:         " << channel << '\n';
    std::cout << "        in use:          " << ((broadcast || p2p) ? "yes" : "no") << '\n';

    if (output) {
        const unsigned xspeed = (v >> 22) & 0x3;
        const unsigned speed = (v >> 14) & 0x3;
        const unsigned overhead = (v >> 10) & 0xf;
        std::cout << "        output speed:    " << speed << '\n';
        std::cout << "        output x-speed:  " << xspeed << '\n';
        std::cout << "        overhead ID:     " << overhead << '\n';
    }
}

} // namespace

int main() {
    std::cout << "macfw cmpprobe — read-only IEC 61883 CMP register probe\n\n";

    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) {
        std::cout << "No operational FW 410 unit found.\n";
        return 2;
    }

    std::cout << "FW410 operational unit:\n";
    std::cout << "    generation: " << device.generation() << '\n';
    std::cout << "    remote node: 0x" << std::hex << device.nodeID()
              << std::dec << '\n';

    const IOReturn openResult = device.open();
    if (openResult != kIOReturnSuccess) {
        std::cout << "    open failed: 0x" << std::hex << openResult
                  << std::dec << '\n';
        return 3;
    }

    std::uint32_t ompr = 0;
    std::uint32_t opcr0 = 0;
    std::uint32_t impr = 0;
    std::uint32_t ipcr0 = 0;

    const bool ok = readReg(device, kOmprLo, ompr) &&
                    readReg(device, kOpcr0Lo, opcr0) &&
                    readReg(device, kImprLo, impr) &&
                    readReg(device, kIpcr0Lo, ipcr0);

    if (!ok)
        return 4;

    std::cout << "    CMP registers (device-relative):\n";
    printMpr("oMPR", ompr);
    printPcr("oPCR[0] (device OUTPUT / host capture)", opcr0, true);
    printMpr("iMPR", impr);
    printPcr("iPCR[0] (device INPUT / host playback)", ipcr0, false);

    return 0;
}
