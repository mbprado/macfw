#include "macfw/amdtp_pcm_stream.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"
#include "macfw/pcm_ring_buffer.h"
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
constexpr std::size_t kHalfPackets = 320;
constexpr std::size_t kPlaybackPcmChannels = 10;
constexpr std::size_t kPlaybackPcmCapacityFrames = 16384;
constexpr std::size_t kCapturePrefillFrames = 4096;
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
    const UInt32 initialCycle = cycleCount(cycleTime);
    const UInt32 firstCycle = (initialCycle + kCycleLead) % kCyclesPerSecond;

    // The FW410 capture side only stays in the sample-bearing operating state
    // when valid host->device AMDTP continues flowing. Use the same continuously
    // serviced 48 kHz scheduler as the proven playback path, with an empty PCM
    // FIFO so it emits correctly timed digital silence.
    macfw::PcmRingBuffer playbackPcm(kPlaybackPcmCapacityFrames, kPlaybackPcmChannels);
    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = macfw::AmdtpTransmitRing::createSilence48k(device, firstCycle, kPlaybackSlots);
    auto capture = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::DeviceToHost, kCaptureMaxPacket);
    auto playback = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::HostToDevice, kPlaybackMaxPacket);
    if (!playbackPcm.valid() || !rx || !tx || !capture || !playback) return false;

    macfw::AmdtpPcmStream48k playbackStreamer(
        tx, playbackPcm, initialCycle, firstCycle, kHalfPackets);
    if (!playbackStreamer.valid() || !playbackStreamer.prime()) return false;

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
    bool captureReady = false;
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
              << "playback keepalive: native 48 kHz AMDTP scheduler, digital silence\n"
              << "capture order: Analog In 1, Analog In 2, S/PDIF L, S/PDIF R\n"
              << "capture prefill: " << kCapturePrefillFrames
              << " frames (~85 ms), armed after CoreAudio ReadInput begins\n"
              << "run ../captureprobe/captureprobe in another terminal; Ctrl-C to stop\n";

    {
        CFAbsoluteTime lastStatus = CFAbsoluteTimeGetCurrent();
        std::uint64_t lastFrames = 0;
        while (!gStopRequested) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0005, false);

            UInt32 nowCycleTime = 0;
            if ((*native)->GetCycleTime(native, &nowCycleTime) == kIOReturnSuccess)
                playbackStreamer.service(cycleCount(nowCycleTime));

            capturePump.service(rx, *captureShared.ring());

            if (!captureReady && captureShared.activateForConsumer(kCapturePrefillFrames)) {
                captureReady = true;
                std::cout << "capture prefill ready: CoreAudio consumer detected; live capture enabled\n";
            }

            const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
            if (now - lastStatus >= 2.0) {
                const auto frames = captureShared.ring()->decodedFrames.load(std::memory_order_acquire);
                const auto& txStats = playbackStreamer.stats();
                std::cout << "capture frames=" << frames
                          << " (delta " << (frames - lastFrames) << ")"
                          << " active=" << captureShared.ring()->active.load(std::memory_order_acquire)
                          << " queued=" << macfw::hal::capture::availableFrames(*captureShared.ring())
                          << " drops=" << captureShared.ring()->droppedFrames.load(std::memory_order_acquire)
                          << " malformed=" << captureShared.ring()->malformedPackets.load(std::memory_order_acquire)
                          << " invalid=" << captureShared.ring()->invalidLabels.load(std::memory_order_acquire)
                          << " hal-read=" << captureShared.ring()->halReadCalls.load(std::memory_order_acquire)
                          << " tx-late=" << txStats.lateCyclePolls
                          << " tx-silence=" << txStats.framesSilenced
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
