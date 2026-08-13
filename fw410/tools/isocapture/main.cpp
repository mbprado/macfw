#include "macfw/amdtp_receive_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <CoreFoundation/CoreFoundation.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

constexpr UInt32 kCapturePayload48k = 128;
constexpr UInt32 kPlaybackPayload48k = 272;
constexpr std::size_t kPacketCount = 64;

static void dumpPacket(std::size_t index,
                       const macfw::AmdtpReceiveRing::PacketSlot& slot,
                       bool raw) {
    const UInt32 iso = slot.isoHeader;
    const unsigned packetLen = iso >> 16;
    const unsigned tag = (iso >> 14) & 0x3;
    const unsigned sy = iso & 0xf;

    std::cout << "    packet " << index
              << ": len=" << packetLen
              << " tag=" << tag
              << " sy=" << sy
              << " status=0x" << std::hex << slot.status
              << " timestamp=0x" << slot.timestamp << std::dec;

    const auto packet = slot.packet();
    if (packet.hasCip()) {
        const auto cip = packet.cip();
        std::cout << " CIP{sid=" << static_cast<unsigned>(cip.sid)
                  << " dbs=" << static_cast<unsigned>(cip.dbs)
                  << " dbc=" << static_cast<unsigned>(cip.dbc)
                  << " fmt=0x" << std::hex << static_cast<unsigned>(cip.fmt)
                  << " fdf=0x" << static_cast<unsigned>(cip.fdf)
                  << " syt=0x" << cip.syt << std::dec << "}";
    }
    std::cout << '\n';

    if (raw && packetLen > 0 && packetLen <= slot.capacity) {
        const unsigned shown = packetLen < 48 ? packetLen : 48;
        std::cout << "        raw:";
        for (unsigned i = 0; i < shown; ++i) {
            std::cout << ' ' << std::hex << std::setw(2)
                      << std::setfill('0')
                      << static_cast<unsigned>(slot.payload[i]);
        }
        std::cout << std::dec << std::setfill(' ');
        if (shown < packetLen)
            std::cout << " ...";
        std::cout << '\n';
    }
}

static void dumpDiagnostics(const macfw::AmdtpReceiveRing& ring, bool raw) {
    const std::size_t touched = ring.touchedCount();

    std::cout << "capture diagnostics:\n";
    std::cout << "    touched NuDCL slots: "
              << touched << " / " << ring.packetCount() << '\n';

    std::size_t shown = 0;
    for (std::size_t i = 0;
         i < ring.packetCount() && shown < 16;
         ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched())
            continue;
        dumpPacket(i, slot, raw);
        ++shown;
    }

    if (touched == 0) {
        std::cout
            << "    interpretation: no receive slot was modified by the ISO DMA engine\n";
    } else if (touched < ring.packetCount()) {
        std::cout
            << "    interpretation: ISO receive is active, but the finite NuDCL burst did not complete\n";
    } else {
        std::cout
            << "    interpretation: all slots changed; callback/finalization is the likely issue\n";
    }
}

static bool run(bool execute, bool raw) {
    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) {
        std::cout << "No operational FW 410 unit found.\n";
        return false;
    }

    std::cout << "FW410 operational unit:\n";
    std::cout << "    generation: " << device.generation() << '\n';
    std::cout << "    remote node: 0x"
              << std::hex << device.nodeID() << std::dec << '\n';

    if (device.open() != kIOReturnSuccess) {
        std::cout << "open failed\n";
        return false;
    }

    std::uint32_t opcr0 = 0;
    std::uint32_t ipcr0 = 0;

    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) {
        std::cout << "PCR read failed\n";
        return false;
    }

    std::cout << "preflight (48 kHz capture):\n";
    std::cout << "    oPCR[0]: 0x" << std::hex << opcr0 << std::dec << '\n';
    std::cout << "    iPCR[0]: 0x" << std::hex << ipcr0 << std::dec << '\n';
    std::cout << "    packet buffer: " << kCapturePayload48k << " bytes\n";
    std::cout << "    packet slots:  " << kPacketCount << '\n';

    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n";
        return false;
    }

    if (!execute) {
        std::cout << "status: PASS - no ISO resources allocated and no stream started\n";
        std::cout << "to execute: ./isocapture --execute [--raw]\n";
        return true;
    }

    auto ring = macfw::AmdtpReceiveRing::create(
        device, kPacketCount, kCapturePayload48k);

    auto capture = macfw::IsochAllocation::create(
        device,
        macfw::IsochAllocation::Direction::DeviceToHost,
        kCapturePayload48k);

    auto playback = macfw::IsochAllocation::create(
        device,
        macfw::IsochAllocation::Direction::HostToDevice,
        kPlaybackPayload48k);

    if (!ring || !capture || !playback) {
        std::cout << "ISO resource creation failed\n";
        return false;
    }

    auto captureChannel = capture.nativeChannel();
    (*captureChannel)->AddListener(
        captureChannel,
        reinterpret_cast<IOFireWireLibIsochPortRef>(ring.nativeLocalPort()));

    auto native = device.nativeHandle();
    bool callbackDispatcher = false;
    bool isochDispatcher = false;
    bool notifications = false;
    bool captureStarted = false;
    bool opcrConnected = false;
    bool ipcrConnected = false;
    bool ok = false;

    if ((*native)->AddCallbackDispatcherToRunLoop(
            native, CFRunLoopGetCurrent()) == kIOReturnSuccess)
        callbackDispatcher = true;

    if ((*native)->AddIsochCallbackDispatcherToRunLoop(
            native, CFRunLoopGetCurrent()) == kIOReturnSuccess)
        isochDispatcher = true;

    if ((*native)->TurnOnNotification(native))
        notifications = true;

    IOReturn kr = capture.allocate();
    if (kr != kIOReturnSuccess) {
        std::cout << "capture ISO resource: failed (0x"
                  << std::hex << kr << std::dec << ")\n";
        goto cleanup;
    }

    std::cout << "capture ISO resource: channel="
              << capture.channel()
              << " speed=" << static_cast<unsigned>(capture.speed())
              << '\n';

    kr = playback.allocate();
    if (kr != kIOReturnSuccess) {
        std::cout << "playback ISO resource: failed (0x"
                  << std::hex << kr << std::dec << ")\n";
        goto cleanup;
    }

    std::cout << "playback ISO resource: channel="
              << playback.channel()
              << " speed=" << static_cast<unsigned>(playback.speed())
              << '\n';

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

    std::cout << "CMP: both directions connected\n";

    kr = (*captureChannel)->Start(captureChannel);
    if (kr != kIOReturnSuccess) {
        std::cout << "capture channel start failed: 0x"
                  << std::hex << kr << std::dec << '\n';
        goto cleanup;
    }
    captureStarted = true;

    std::cout << "capture local isoch channel: started\n";
    std::cout << "capture: waiting for " << kPacketCount
              << " ISO packets (2 s timeout)\n";

    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false);

    if (ring.completed())
        std::cout << "capture: NuDCL burst completed\n";
    else
        std::cout << "capture: timeout before NuDCL burst completed\n";

    dumpDiagnostics(ring, raw);
    ok = ring.touchedCount() > 0;

cleanup:
    if (captureStarted)
        (*captureChannel)->Stop(captureChannel);

    if (ipcrConnected) {
        const auto restore = macfw::cmp::restore(
            device, macfw::cmp::kIpcr0AddressLo, ipcr0);
        std::cout << "restore iPCR[0]: "
                  << (restore == kIOReturnSuccess ? "success" : "failed")
                  << '\n';
    }

    if (opcrConnected) {
        const auto restore = macfw::cmp::restore(
            device, macfw::cmp::kOpcr0AddressLo, opcr0);
        std::cout << "restore oPCR[0]: "
                  << (restore == kIOReturnSuccess ? "success" : "failed")
                  << '\n';
    }

    playback.release();
    capture.release();

    if (notifications)
        (*native)->TurnOffNotification(native);
    if (isochDispatcher)
        (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (callbackDispatcher)
        (*native)->RemoveCallbackDispatcherFromRunLoop(native);

    std::uint32_t opAfter = 0;
    std::uint32_t ipAfter = 0;
    if (macfw::cmp::readOpcr0(device, opAfter) == kIOReturnSuccess &&
        macfw::cmp::readIpcr0(device, ipAfter) == kIOReturnSuccess) {
        std::cout << "post-test PCR restore: "
                  << ((opAfter == opcr0 && ipAfter == ipcr0)
                          ? "PASS" : "FAIL")
                  << '\n';
    }

    return ok;
}

} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    bool raw = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute")
            execute = true;
        else if (arg == "--raw")
            raw = true;
        else {
            std::cerr << "usage: ./isocapture [--execute] [--raw]\n";
            return 64;
        }
    }

    std::cout << "macfw isocapture — guarded FW410 AMDTP capture sniffer\n\n";
    return run(execute, raw) ? 0 : 1;
}
