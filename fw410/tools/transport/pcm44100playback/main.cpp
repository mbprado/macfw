#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <CoreFoundation/CoreFoundation.h>
#include <cstdint>
#include <iostream>
#include <string>

namespace {
constexpr UInt32 kCaptureMaxPacket44100 = 168;
constexpr UInt32 kPlaybackMaxPacket44100 = 360;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kPlaybackSlots = 128;
constexpr UInt32 kCycleLead = 256;

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

bool run(bool execute) {
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
              << "    packet scheduler:   rational 441/640\n"
              << "    PCM events/packet:  8\n"
              << "    tone:               1 kHz on Analog Out 1 (PCM position 2)\n"
              << "    amplitude:          ~-36 dBFS\n";

    if (!execute) {
        std::cout << "status: PASS - dry run only; no stream started\n"
                  << "to execute safely: ./run44100.sh\n";
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
              << "observation window: 2.0 s\n"
              << "listen for 1 kHz on Analog Out 1\n";
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false);
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
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else {
            std::cerr << "usage: ./pcm44100playback [--execute]\n";
            return 64;
        }
    }

    std::cout << "macfw pcm44100playback — native FW410 44.1 kHz PCM playback test\n\n";
    return run(execute) ? 0 : 1;
}
