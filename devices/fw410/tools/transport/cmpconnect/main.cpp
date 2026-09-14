#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

constexpr UInt32 kCapturePayload48k = 128;
constexpr UInt32 kPlaybackPayload48k = 272;

static void printPcr(const char* name, std::uint32_t value) {
    const auto state = macfw::cmp::decodePcr(value);

    std::cout << "    " << name << ": 0x"
              << std::hex << std::setw(8) << std::setfill('0')
              << value
              << std::dec << std::setfill(' ') << '\n';

    std::cout << "        online: "
              << (state.online ? "yes" : "no")
              << ", p2p="
              << static_cast<unsigned>(state.p2pConnections)
              << ", channel="
              << static_cast<unsigned>(state.channel)
              << '\n';
}

static bool readPcrs(macfw::FireWireDevice& device,
                     std::uint32_t& opcr0,
                     std::uint32_t& ipcr0) {
    const IOReturn op = macfw::cmp::readOpcr0(device, opcr0);
    const IOReturn ip = macfw::cmp::readIpcr0(device, ipcr0);

    if (op != kIOReturnSuccess || ip != kIOReturnSuccess) {
        std::cout << "PCR read failed: oPCR=0x"
                  << std::hex << op
                  << " iPCR=0x" << ip
                  << std::dec << '\n';
        return false;
    }
    return true;
}

static bool run(bool execute) {
    auto device = macfw::FireWireDevice::findByProductName("FW 410");

    if (!device) {
        std::cout << "No operational FW 410 unit found.\n";
        return false;
    }

    std::cout << "FW410 operational unit:\n";
    std::cout << "    generation: " << device.generation() << '\n';
    std::cout << "    remote node: 0x"
              << std::hex << device.nodeID()
              << std::dec << '\n';

    const IOReturn openResult = device.open();
    if (openResult != kIOReturnSuccess) {
        std::cout << "open failed: 0x"
                  << std::hex << openResult
                  << std::dec << '\n';
        return false;
    }

    std::uint32_t opcr0 = 0;
    std::uint32_t ipcr0 = 0;

    if (!readPcrs(device, opcr0, ipcr0))
        return false;

    std::cout << "preflight (48 kHz formation only):\n";
    printPcr("oPCR[0] device OUTPUT / host capture", opcr0);
    printPcr("iPCR[0] device INPUT / host playback", ipcr0);
    std::cout << "    capture max payload:  "
              << kCapturePayload48k << " bytes\n";
    std::cout << "    playback max payload: "
              << kPlaybackPayload48k << " bytes\n";

    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cout
            << "status: REFUSED - one or both PCR0 plugs "
               "are offline or already in use\n";
        return false;
    }

    if (!execute) {
        std::cout
            << "status: PASS - no resources allocated "
               "and no PCR writes performed\n";
        std::cout << "to execute: ./cmpconnect --execute\n";
        return true;
    }

    auto capture = macfw::IsochAllocation::create(
        device,
        macfw::IsochAllocation::Direction::DeviceToHost,
        kCapturePayload48k);

    auto playback = macfw::IsochAllocation::create(
        device,
        macfw::IsochAllocation::Direction::HostToDevice,
        kPlaybackPayload48k);

    if (!capture || !playback) {
        std::cout << "resource objects: failed to create/configure\n";
        return false;
    }

    IOReturn kr = capture.allocate();
    if (kr != kIOReturnSuccess) {
        std::cout << "capture IRM allocation: failed (0x"
                  << std::hex << kr << std::dec << ")\n";
        return false;
    }

    std::cout << "capture IRM allocation: success, channel="
              << capture.channel()
              << ", speed="
              << static_cast<unsigned>(capture.speed())
              << '\n';

    kr = playback.allocate();
    if (kr != kIOReturnSuccess) {
        std::cout << "playback IRM allocation: failed (0x"
                  << std::hex << kr << std::dec << ")\n";
        return false;
    }

    std::cout << "playback IRM allocation: success, channel="
              << playback.channel()
              << ", speed="
              << static_cast<unsigned>(playback.speed())
              << '\n';

    bool opcrConnected = false;
    bool ipcrConnected = false;
    bool ok = false;

    kr = macfw::cmp::connectOpcr0(
        device, opcr0, capture.channel(), capture.speed());
    if (kr != kIOReturnSuccess) {
        std::cout << "connect oPCR[0]: failed (0x"
                  << std::hex << kr << std::dec << ")\n";
        goto cleanup;
    }
    opcrConnected = true;

    kr = macfw::cmp::connectIpcr0(
        device, ipcr0, playback.channel());
    if (kr != kIOReturnSuccess) {
        std::cout << "connect iPCR[0]: failed (0x"
                  << std::hex << kr << std::dec << ")\n";
        goto cleanup;
    }
    ipcrConnected = true;

    std::cout << "CMP establish: both PCR0 connections set\n";

    {
        std::uint32_t op = 0;
        std::uint32_t ip = 0;
        if (readPcrs(device, op, ip)) {
            printPcr("oPCR[0] connected", op);
            printPcr("iPCR[0] connected", ip);
        }
    }

    std::cout << "AMDTP start: NO\n";
    usleep(500000);
    ok = true;

cleanup:
    if (ipcrConnected) {
        const IOReturn restore =
            macfw::cmp::restore(
                device, macfw::cmp::kIpcr0AddressLo, ipcr0);
        std::cout << "restore iPCR[0]: "
                  << (restore == kIOReturnSuccess ? "success" : "failed")
                  << '\n';
    }

    if (opcrConnected) {
        const IOReturn restore =
            macfw::cmp::restore(
                device, macfw::cmp::kOpcr0AddressLo, opcr0);
        std::cout << "restore oPCR[0]: "
                  << (restore == kIOReturnSuccess ? "success" : "failed")
                  << '\n';
    }

    {
        const IOReturn release = playback.release();
        std::cout << "release playback IRM: 0x"
                  << std::hex << release << std::dec << '\n';
    }

    {
        const IOReturn release = capture.release();
        std::cout << "release capture IRM:  0x"
                  << std::hex << release << std::dec << '\n';
    }

    {
        std::uint32_t op = 0;
        std::uint32_t ip = 0;

        if (readPcrs(device, op, ip)) {
            std::cout << "post-test PCR state:\n";
            printPcr("oPCR[0]", op);
            printPcr("iPCR[0]", ip);
            std::cout << "    exact restore: "
                      << ((op == opcr0 && ip == ipcr0) ? "PASS" : "FAIL")
                      << '\n';
        }
    }

    return ok;
}

} // namespace

int main(int argc, char** argv) {
    const bool execute =
        argc == 2 && std::string(argv[1]) == "--execute";

    if (argc > 2 || (argc == 2 && !execute)) {
        std::cerr << "usage: ./cmpconnect [--execute]\n";
        return 64;
    }

    std::cout
        << "macfw cmpconnect — guarded dual CMP connection test\n\n";

    return run(execute) ? 0 : 1;
}
