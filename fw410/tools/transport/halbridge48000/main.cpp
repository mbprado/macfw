#include "macfw/amdtp_pcm_stream.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/firewire_device.h"
#include "macfw/pcm_ring_buffer.h"
#include "macfw_hal_shm.h"
#include "../capture_shared.h"
#include "../full_duplex_shared.h"
#include "../full_duplex_lifecycle.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <pthread.h>
#include <vector>

namespace {
using namespace macfw::transport::duplex;
constexpr UInt32 kCycleLead = 256;

volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }

void requestInteractiveQos() {
    const int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (rc == 0)
        std::cout << "transport thread QoS: user-interactive\n";
    else
        std::cout << "transport thread QoS: request failed (" << rc << ")\n";
}

bool run() {
    SharedPlaybackReader input;
    if (!input.open()) {
        std::cerr << "shared HAL playback ring not available; install/restart the HAL plug-in first\n";
        return false;
    }
    if (input.ring()->sampleRate.load(std::memory_order_acquire) != 48000) {
        std::cerr << "HAL device must be set to 48000 Hz for this engine\n";
        return false;
    }
    input.discardBacklog();

    macfw::transport::CaptureSharedWriter captureShared;
    if (!captureShared.open(48000)) {
        std::cerr << "capture shared ring setup failed\n";
        return false;
    }
    macfw::transport::CaptureReceivePump capturePump(0x02);

    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) { std::cerr << "No operational FW 410 unit found.\n"; return false; }
    if (device.open() != kIOReturnSuccess) return false;

    auto native = device.nativeHandle();
    UInt32 ct = 0;
    if ((*native)->GetCycleTime(native, &ct) != kIOReturnSuccess) return false;
    const UInt32 initialCycle = cycleCount(ct);
    const UInt32 firstCycle = (initialCycle + kCycleLead) % kCyclesPerSecond;

    macfw::PcmRingBuffer pcm(kPcmCapacityFrames, kPcmChannels);
    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = macfw::AmdtpTransmitRing::createSilence48k(device, firstCycle, kPlaybackSlots);
    if (!pcm.valid() || !rx || !tx) return false;

    macfw::AmdtpPcmStream48k streamer(tx, pcm, initialCycle, firstCycle, kHalfPackets);
    if (!streamer.valid() || !streamer.prime()) return false;

    FireWireDuplexLifecycle lifecycle;
    if (!lifecycle.prepare(device, rx, tx.nativeLocalPort(),
                           kCaptureMaxPacket, kPlaybackMaxPacket))
        return false;
    if (!lifecycle.addCallbackDispatcher()) return false;
    if (!lifecycle.startIsoch()) return false;

    std::vector<float> audio(4096 * macfw::hal::kChannels, 0.0f);
    std::vector<std::int32_t> mapped(4096*kPcmChannels,0);
    std::uint64_t lastWrite=input.ring()->writeFrame.load(std::memory_order_acquire);
    std::uint64_t lastCaptureFrames=0;
    CFAbsoluteTime lastStatus=CFAbsoluteTimeGetCurrent();
    bool captureReady=false;

    requestInteractiveQos();
    std::cout << "duplex ISO started\n"
              << "TX ring: 640 cycles / 320-cycle refill halves\n"
              << "PCM FIFO: 16384 frames\n"
              << "CoreAudio outputs: Analog 1-8, S/PDIF L/R (10 channels)\n"
              << "CoreAudio inputs: Analog In 1-2, S/PDIF In L/R (4 channels)\n"
              << "capture prefill: " << kCapturePrefillFrames << " frames (~85 ms)\n"
              << "capture receive: terminal-slot completed 32-cycle chunks\n"
              << "HAL bridge active: full-duplex 48 kHz playback + capture; Ctrl-C to stop\n";

    while (!gStopRequested) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.00025,false);

        capturePump.service(rx, *captureShared.ring());

        drainPlayback(*input.ring(),pcm,audio,mapped);
        UInt32 nowCt=0;
        if ((*native)->GetCycleTime(native,&nowCt)==kIOReturnSuccess)
            streamer.service(cycleCount(nowCt));

        capturePump.service(rx, *captureShared.ring());

        if (!captureReady && captureShared.activateForConsumer(kCapturePrefillFrames)) {
            captureReady=true;
            std::cout << "capture prefill ready: CoreAudio consumer detected; live capture enabled\n";
        }

        const CFAbsoluteTime now=CFAbsoluteTimeGetCurrent();
        if (now-lastStatus>=2.0) {
            const auto w=input.ring()->writeFrame.load(std::memory_order_acquire);
            const auto captureFrames=captureShared.ring()->decodedFrames.load(std::memory_order_acquire);
            const auto& txStats=streamer.stats();
            const auto& rxStats=capturePump.stats();
            std::cout << "HAL out=" << w << " (delta " << (w-lastWrite) << ")"
                      << " shared=" << macfw::hal::availableFrames(*input.ring())
                      << " pcm=" << pcm.availableFrames()
                      << " out-drops=" << input.ring()->droppedFrames.load()
                      << " tx-late=" << txStats.lateCyclePolls
                      << " tx-silence=" << txStats.framesSilenced
                      << " | capture=" << captureFrames << " (delta " << (captureFrames-lastCaptureFrames) << ")"
                      << " active=" << captureShared.ring()->active.load(std::memory_order_acquire)
                      << " queued=" << macfw::hal::capture::availableFrames(*captureShared.ring())
                      << " in-drops=" << captureShared.ring()->droppedFrames.load(std::memory_order_acquire)
                      << " malformed=" << captureShared.ring()->malformedPackets.load(std::memory_order_acquire)
                      << " invalid=" << captureShared.ring()->invalidLabels.load(std::memory_order_acquire)
                      << " chunks=" << rxStats.completedChunks
                      << " dbc-gap=" << rxStats.dbcDiscontinuities
                      << " ts-back=" << rxStats.timestampRegressions
                      << " reorder=" << rxStats.reorderedPackets
                      << " stale=" << rxStats.stalePackets
                      << " hal-read=" << captureShared.ring()->halReadCalls.load(std::memory_order_acquire)
                      << '\n';
            lastWrite=w;
            lastCaptureFrames=captureFrames;
            lastStatus=now;
        }
    }

    std::cout << "stop requested; restoring ISO/CMP resources\n";
    lifecycle.stop();
    return true;
}
} // namespace

int halbridge48000_inner_main() {
    gStopRequested = 0;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::cout << "macfw halbridge48000 — native 48 kHz full-duplex CoreAudio HAL to FW410 transport\n";
    return run() ? 0 : 1;
}
