#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"
#include "../capture_shared.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <csignal>
#include <cstdint>
#include <iostream>

namespace {
constexpr UInt32 kCaptureMaxPacket = 168;
constexpr UInt32 kPlaybackMaxPacket = 360;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kPlaybackSlots = 640;
constexpr UInt32 kCycleLead = 256;
constexpr UInt32 kCyclesPerSecond = 8000;

volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }
UInt32 cycleCount(UInt32 cycleTime) { return (cycleTime >> 12) & 0x1fffu; }

bool run() {
    macfw::transport::CaptureSharedWriter captureShared;
    if (!captureShared.open(48000)) {
        std::cerr << "capture shared ring setup failed\n";
        return false;
    }
    macfw::transport::CaptureReceivePump capturePump(0x02);

    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) {
        std::cerr << "No operational FW 410 unit found.\n";
        return false;
    }
    if (device.open() != kIOReturnSuccess) return false;

    std::uint32_t opcr0 = 0, ipcr0 = 0;
    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) return false;
    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cerr << "PCR0 offline or already connected; stop haltransport before this isolated capture test\n";
        return false;
    }

    auto native = device.nativeHandle();
    UInt32 cycleTime = 0;
    if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) return false;
    const UInt32 firstCycle = (cycleCount(cycleTime) + kCycleLead) % kCyclesPerSecond;

    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = macfw::AmdtpTransmitRing::createSilence48k(device, firstCycle, kPlaybackSlots);
    auto capture = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::DeviceToHost, kCaptureMaxPacket);
    auto playback = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::HostToDevice, kPlaybackMaxPacket);
    if (!rx || !tx || !capture || !playback) return false;

    (*capture.nativeChannel())->AddListener(
        capture.nativeChannel(),
        reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
    if (playback.bindHostToDeviceTalkerFirst(tx.nativeLocalPort()) != kIOReturnSuccess)
        return false;

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

    if (capture.allocate() != kIOReturnSuccess || playback.allocate() != kIOReturnSuccess)
        goto cleanup;
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

    std::cout << "macfw capturebridge48000 — FW410 capture to shared memory\n"
              << "duplex ISO started\n"
              << "capture order: Analog In 1, Analog In 2, S/PDIF L, S/PDIF R\n"
              << "run ../captureprobe/captureprobe in another terminal; Ctrl-C to stop\n";

    {
        CFAbsoluteTime lastStatus = CFAbsoluteTimeGetCurrent();
        std::uint64_t lastFrames = 0;
        while (!gStopRequested) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0005, false);
            capturePump.service(rx, *captureShared.ring());
            const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
            if (now - lastStatus >= 2.0) {
                const auto frames = captureShared.ring()->decodedFrames.load(std::memory_order_acquire);
                std::cout << "capture frames=" << frames
                          << " (delta " << (frames - lastFrames) << ")"
                          << " queued=" << macfw::hal::capture::availableFrames(*captureShared.ring())
                          << " drops=" << captureShared.ring()->droppedFrames.load(std::memory_order_acquire)
                          << " malformed=" << captureShared.ring()->malformedPackets.load(std::memory_order_acquire)
                          << " invalid=" << captureShared.ring()->invalidLabels.load(std::memory_order_acquire)
                          << '\n';
                lastFrames = frames;
                lastStatus = now;
            }
        }
    }

    std::cout << "stop requested; restoring ISO/CMP resources\n";
    ok = true;

cleanup:
    if (playbackStarted) (*playback.nativeChannel())->Stop(playback.nativeChannel());
    if (captureStarted) (*capture.nativeChannel())->Stop(capture.nativeChannel());
    if (ipConnected) macfw::cmp::restore(device, macfw::cmp::kIpcr0AddressLo, ipcr0);
    if (opConnected) macfw::cmp::restore(device, macfw::cmp::kOpcr0AddressLo, opcr0);
    playback.release();
    capture.release();
    if (notifications) (*native)->TurnOffNotification(native);
    if (isochDispatcher) (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (callbackDispatcher) (*native)->RemoveCallbackDispatcherFromRunLoop(native);
    return ok;
}
} // namespace

int main() {
    gStopRequested = 0;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    return run() ? 0 : 1;
}
