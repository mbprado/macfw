#include "macfw/amdtp_packet.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

namespace {
constexpr UInt32 kCaptureMaxPacket44100 = 168;
constexpr UInt32 kPlaybackMaxPacket44100 = 360;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kPlaybackSlots = 4096; // 512 ms at 8k FireWire cycles/s.
constexpr UInt32 kCycleLead = 256;
constexpr double kObservationSeconds = 0.35; // stop before first TX-ring wrap.

void dumpTx(const macfw::AmdtpTransmitRing& tx, bool raw) {
    std::size_t data = 0;
    std::size_t nodata = 0;
    for (std::size_t i = 0; i < tx.packetCount(); ++i) {
        if (tx.slot(i).dataBearing) ++data;
        else ++nodata;
    }

    std::cout << "TX schedule summary:\n"
              << "    packets:            " << tx.packetCount() << '\n'
              << "    data-bearing:       " << data << '\n'
              << "    NODATA:             " << nodata << '\n';

    if (!raw) return;

    const std::size_t count = std::min<std::size_t>(32, tx.packetCount());
    std::cout << "TX first " << count << " packets:\n";
    for (std::size_t i = 0; i < count; ++i) {
        const auto& slot = tx.slot(i);
        const macfw::amdtp::PacketView view{slot.payload, slot.length};
        const auto cip = view.cip();
        std::cout << "    packet " << i
                  << ": cycle=" << slot.cycle
                  << " len=" << slot.length
                  << " CIP{dbs=" << static_cast<unsigned>(cip.dbs)
                  << " dbc=" << static_cast<unsigned>(cip.dbc)
                  << " fmt=0x" << std::hex << static_cast<unsigned>(cip.fmt)
                  << " fdf=0x" << static_cast<unsigned>(cip.fdf)
                  << " syt=0x" << cip.syt << std::dec << "}\n"
                  << "        raw:";
        const std::size_t bytes = std::min<std::size_t>(slot.length, 48);
        for (std::size_t b = 0; b < bytes; ++b) {
            std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(slot.payload[b]);
        }
        std::cout << std::dec << std::setfill(' ') << '\n';
    }
}

void dumpCapture(const macfw::AmdtpReceiveRing& ring) {
    std::size_t touched = 0;
    std::size_t dataBearing = 0;
    std::size_t fdf44100 = 0;
    for (std::size_t i = 0; i < ring.packetCount(); ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched()) continue;
        ++touched;
        const auto packet = slot.packet();
        if (packet.length > 8) ++dataBearing;
        if (packet.hasCip() && packet.cip().fdf == 0x01) ++fdf44100;
    }
    std::cout << "capture summary:\n"
              << "    touched slots:      " << touched << " / " << ring.packetCount() << '\n'
              << "    data-bearing slots: " << dataBearing << '\n'
              << "    FDF=0x01 slots:     " << fdf44100 << '\n'
              << "result: " << (dataBearing && fdf44100 ? "PASS" : "FAIL")
              << (dataBearing ? " - native 44.1 kHz sample-bearing capture observed" : " - no sample-bearing capture")
              << '\n';
}

bool run(bool execute, bool raw) {
    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) {
        std::cout << "No operational FW 410 unit found.\n";
        return false;
    }

    std::cout << "FW410 operational unit:\n"
              << "    generation: " << device.generation() << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID() << std::dec << '\n';

    if (device.open() != kIOReturnSuccess) {
        std::cout << "open failed\n";
        return false;
    }

    std::uint32_t opcr0 = 0, ipcr0 = 0;
    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) {
        std::cout << "PCR read failed\n";
        return false;
    }
    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n";
        return false;
    }

    auto native = device.nativeHandle();
    UInt32 cycleTime = 0;
    if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess)
        return false;
    const UInt32 now = (cycleTime >> 12) & 0x1fffu;
    const UInt32 firstCycle = (now + kCycleLead) & 0x1fffu;

    std::cout << "native 44.1 kHz playback test:\n"
              << "    sample rate:        44100 Hz\n"
              << "    FDF:                0x01\n"
              << "    packet scheduler:   blocking base-44.1 SYT sequence\n"
              << "    PCM events/packet:  8\n"
              << "    TX program:         4096 cycles (512 ms), stopped before wrap\n"
              << "    tone:               1 kHz on Analog Out 1 (PCM position 2)\n"
              << "    amplitude:          ~-36 dBFS\n";

    if (!execute) {
        std::cout << "status: PASS - dry run only; no stream started\n"
                  << "to execute safely: ./run44100.sh [--raw]\n";
        return true;
    }

    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket44100);
    auto tx = macfw::AmdtpTransmitRing::createTone44100(
        device, firstCycle, 2, 1000.0, 131072.0, kPlaybackSlots);
    auto capture = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::DeviceToHost, kCaptureMaxPacket44100);
    auto playback = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::HostToDevice, kPlaybackMaxPacket44100);

    if (!rx || !tx || !capture || !playback) {
        std::cout << "transport object creation failed\n";
        return false;
    }

    dumpTx(tx, raw);

    (*capture.nativeChannel())->AddListener(
        capture.nativeChannel(),
        reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
    if (playback.bindHostToDeviceTalkerFirst(tx.nativeLocalPort()) != kIOReturnSuccess) {
        std::cout << "playback port binding failed\n";
        return false;
    }

    bool callbackDispatcher = false;
    bool isochDispatcher = false;
    bool notifications = false;
    bool captureStarted = false;
    bool playbackStarted = false;
    bool opConnected = false;
    bool ipConnected = false;
    bool ok = false;

    if ((*native)->AddCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess)
        callbackDispatcher = true;
    if ((*native)->AddIsochCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess)
        isochDispatcher = true;
    if ((*native)->TurnOnNotification(native)) notifications = true;

    if (capture.allocate() != kIOReturnSuccess || playback.allocate() != kIOReturnSuccess) {
        std::cout << "ISO resource allocation failed\n";
        goto cleanup;
    }

    std::cout << "ISO resources:\n"
              << "    capture channel:  " << capture.channel() << '\n'
              << "    playback channel: " << playback.channel() << '\n';

    if (macfw::cmp::connectOpcr0(device, opcr0, capture.channel(), capture.speed()) != kIOReturnSuccess)
        goto cleanup;
    opConnected = true;
    if (macfw::cmp::connectIpcr0(device, ipcr0, playback.channel()) != kIOReturnSuccess)
        goto cleanup;
    ipConnected = true;

    if ((*playback.nativeChannel())->Start(playback.nativeChannel()) != kIOReturnSuccess)
        goto cleanup;
    playbackStarted = true;
    if ((*capture.nativeChannel())->Start(capture.nativeChannel()) != kIOReturnSuccess)
        goto cleanup;
    captureStarted = true;

    std::cout << "duplex ISO: started (native 44.1 kHz prebuilt PCM)\n"
              << "observation window: " << kObservationSeconds << " s (before TX wrap)\n"
              << "listen for 1 kHz on Analog Out 1\n";
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, kObservationSeconds, false);
    dumpCapture(rx);
    ok = true;

cleanup:
    if (playbackStarted) (*playback.nativeChannel())->Stop(playback.nativeChannel());
    if (captureStarted) (*capture.nativeChannel())->Stop(capture.nativeChannel());

    if (ipConnected) {
        const auto kr = macfw::cmp::restore(device, macfw::cmp::kIpcr0AddressLo, ipcr0);
        std::cout << "restore iPCR[0]: " << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
    }
    if (opConnected) {
        const auto kr = macfw::cmp::restore(device, macfw::cmp::kOpcr0AddressLo, opcr0);
        std::cout << "restore oPCR[0]: " << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
    }

    playback.release();
    capture.release();
    if (notifications) (*native)->TurnOffNotification(native);
    if (isochDispatcher) (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (callbackDispatcher) (*native)->RemoveCallbackDispatcherFromRunLoop(native);

    return ok;
}
} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    bool raw = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else if (arg == "--raw") raw = true;
        else {
            std::cerr << "usage: ./pcm44100playback [--execute] [--raw]\n";
            return 64;
        }
    }

    std::cout << "macfw pcm44100playback — native FW410 44.1 kHz PCM playback test\n\n";
    return run(execute, raw) ? 0 : 1;
}
