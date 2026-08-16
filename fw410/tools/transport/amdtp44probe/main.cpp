#include "macfw/amdtp_nodata_transmit_ring.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <CoreFoundation/CoreFoundation.h>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

namespace {
constexpr UInt32 kCaptureMaxPacket = 168;
constexpr UInt32 kPlaybackMaxPacket = 360;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kPlaybackSlots = 128;
constexpr UInt32 kCycleLead = 256;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr std::uint8_t kFdf44100 = 0x01;

UInt32 cycleCount(UInt32 cycleTime) { return (cycleTime >> 12) & 0x1fffu; }

void dumpPacket(std::size_t index, const macfw::AmdtpReceiveRing::PacketSlot& slot, bool raw) {
    const auto packet = slot.packet();
    const UInt32 iso = slot.isoHeader;
    const unsigned len = iso >> 16;
    const unsigned tag = (iso >> 14) & 0x3;
    const unsigned sy = iso & 0xf;
    std::cout << "    packet " << index << ": len=" << len << " tag=" << tag << " sy=" << sy;
    if (packet.hasCip()) {
        const auto cip = packet.cip();
        std::cout << " CIP{dbs=" << static_cast<unsigned>(cip.dbs)
                  << " dbc=" << static_cast<unsigned>(cip.dbc)
                  << " fmt=0x" << std::hex << static_cast<unsigned>(cip.fmt)
                  << " fdf=0x" << static_cast<unsigned>(cip.fdf)
                  << " syt=0x" << cip.syt << std::dec << "}";
    }
    std::cout << '\n';
    if (raw && len && len <= slot.capacity) {
        const unsigned shown = len < 48 ? len : 48;
        std::cout << "        raw:";
        for (unsigned i = 0; i < shown; ++i)
            std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(slot.payload[i]);
        std::cout << std::dec << std::setfill(' ');
        if (shown < len) std::cout << " ...";
        std::cout << '\n';
    }
}

bool run(bool execute, bool raw) {
    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) { std::cout << "No operational FW 410 unit found.\n"; return false; }
    std::cout << "FW410 operational unit:\n"
              << "    generation: " << device.generation() << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID() << std::dec << '\n';
    if (device.open() != kIOReturnSuccess) { std::cout << "open failed\n"; return false; }

    std::uint32_t opcr0 = 0, ipcr0 = 0;
    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) {
        std::cout << "PCR read failed\n"; return false;
    }
    std::cout << "44.1 kHz characterization:\n"
              << "    prerequisite: FW410 signal format already set to 44100 Hz\n"
              << "    host playback: continuous AMDTP NODATA, FDF=0x01, DBS=11\n"
              << "    capture: observe device packet cadence/DBC/SYT only\n";
    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n"; return false;
    }
    if (!execute) {
        std::cout << "status: PASS - dry run only\n"
                  << "use run44.sh for guarded switch/test/restore\n";
        return true;
    }

    auto native = device.nativeHandle();
    UInt32 cycleTime = 0;
    if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) return false;
    const UInt32 firstCycle = (cycleCount(cycleTime) + kCycleLead) % kCyclesPerSecond;

    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = macfw::AmdtpNoDataTransmitRing::create(device, firstCycle, kFdf44100, 11, kPlaybackSlots);
    auto capture = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::DeviceToHost, kCaptureMaxPacket);
    auto playback = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::HostToDevice, kPlaybackMaxPacket);
    if (!rx || !tx || !capture || !playback) {
        std::cout << "transport object creation failed\n"; return false;
    }

    (*capture.nativeChannel())->AddListener(capture.nativeChannel(),
        reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
    if (playback.bindHostToDeviceTalkerFirst(tx.nativeLocalPort()) != kIOReturnSuccess) {
        std::cout << "playback port binding failed\n"; return false;
    }

    bool cb = false, iso = false, notify = false;
    bool capStarted = false, playStarted = false, opConn = false, ipConn = false;
    bool ok = false;
    if ((*native)->AddCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) cb = true;
    if ((*native)->AddIsochCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) iso = true;
    if ((*native)->TurnOnNotification(native)) notify = true;

    if (capture.allocate() != kIOReturnSuccess || playback.allocate() != kIOReturnSuccess) {
        std::cout << "ISO allocation failed\n"; goto cleanup;
    }
    std::cout << "ISO resources:\n"
              << "    capture channel:  " << capture.channel() << '\n'
              << "    playback channel: " << playback.channel() << '\n';
    if (macfw::cmp::connectOpcr0(device, opcr0, capture.channel(), capture.speed()) != kIOReturnSuccess) goto cleanup;
    opConn = true;
    if (macfw::cmp::connectIpcr0(device, ipcr0, playback.channel()) != kIOReturnSuccess) goto cleanup;
    ipConn = true;
    if ((*playback.nativeChannel())->Start(playback.nativeChannel()) != kIOReturnSuccess) goto cleanup;
    playStarted = true;
    if ((*capture.nativeChannel())->Start(capture.nativeChannel()) != kIOReturnSuccess) goto cleanup;
    capStarted = true;

    std::cout << "duplex ISO: started (44.1 kHz NODATA TX; observing FW410 capture)\n";
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false);

    {
        std::size_t touched = 0, data = 0, shown = 0;
        std::map<unsigned, std::size_t> lengthCounts;
        std::map<unsigned, std::size_t> fdfCounts;
        for (std::size_t i = 0; i < rx.packetCount(); ++i) {
            const auto& s = rx.slot(i);
            if (!s.touched()) continue;
            ++touched;
            const unsigned len = s.isoHeader >> 16;
            ++lengthCounts[len];
            const auto p = s.packet();
            if (len > 8) ++data;
            if (p.hasCip()) ++fdfCounts[p.cip().fdf];
            if (shown < 32) { dumpPacket(i, s, raw); ++shown; }
        }
        std::cout << "capture summary:\n"
                  << "    touched slots:      " << touched << " / " << rx.packetCount() << '\n'
                  << "    data-bearing slots: " << data << '\n'
                  << "    packet lengths:";
        for (const auto& kv : lengthCounts) std::cout << ' ' << kv.first << "B=" << kv.second;
        std::cout << "\n    FDF values:";
        for (const auto& kv : fdfCounts) std::cout << " 0x" << std::hex << kv.first << std::dec << '=' << kv.second;
        std::cout << '\n';
        ok = data > 0;
    }

cleanup:
    if (playStarted) (*playback.nativeChannel())->Stop(playback.nativeChannel());
    if (capStarted) (*capture.nativeChannel())->Stop(capture.nativeChannel());
    if (ipConn) {
        const auto kr = macfw::cmp::restore(device, macfw::cmp::kIpcr0AddressLo, ipcr0);
        std::cout << "restore iPCR[0]: " << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
    }
    if (opConn) {
        const auto kr = macfw::cmp::restore(device, macfw::cmp::kOpcr0AddressLo, opcr0);
        std::cout << "restore oPCR[0]: " << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
    }
    playback.release(); capture.release();
    if (notify) (*native)->TurnOffNotification(native);
    if (iso) (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (cb) (*native)->RemoveCallbackDispatcherFromRunLoop(native);
    return ok;
}
}

int main(int argc, char** argv) {
    bool execute = false, raw = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--execute") execute = true;
        else if (a == "--raw") raw = true;
        else { std::cerr << "usage: ./amdtp44probe [--execute] [--raw]\n"; return 64; }
    }
    std::cout << "macfw amdtp44probe — native FW410 44.1 kHz AMDTP characterization\n\n";
    return run(execute, raw) ? 0 : 1;
}
