#include "macfw/amdtp_pcm_stream.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"
#include "macfw/pcm_ring_buffer.h"
#include "macfw_hal_shm.h"
#include "../capture_shared.h"
#include "../full_duplex_shared.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
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

    std::uint32_t opcr0 = 0, ipcr0 = 0;
    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) return false;
    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cerr << "PCR0 offline or already connected\n";
        return false;
    }

    auto native = device.nativeHandle();
    UInt32 ct = 0;
    if ((*native)->GetCycleTime(native, &ct) != kIOReturnSuccess) return false;
    const UInt32 initialCycle = cycleCount(ct);
    const UInt32 firstCycle = (initialCycle + kCycleLead) % kCyclesPerSecond;

    macfw::PcmRingBuffer pcm(kPcmCapacityFrames, kPcmChannels);
    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = macfw::AmdtpTransmitRing::createSilence48k(device, firstCycle, kPlaybackSlots);
    auto capture = macfw::IsochAllocation::create(device, macfw::IsochAllocation::Direction::DeviceToHost, kCaptureMaxPacket);
    auto playback = macfw::IsochAllocation::create(device, macfw::IsochAllocation::Direction::HostToDevice, kPlaybackMaxPacket);
    if (!pcm.valid() || !rx || !tx || !capture || !playback) return false;

    macfw::AmdtpPcmStream48k streamer(tx, pcm, initialCycle, firstCycle, kHalfPackets);
    if (!streamer.valid() || !streamer.prime()) return false;

    (*capture.nativeChannel())->AddListener(capture.nativeChannel(), reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
    if (playback.bindHostToDeviceTalkerFirst(tx.nativeLocalPort()) != kIOReturnSuccess) return false;

    bool cb=false, iso=false, notif=false, capStart=false, playStart=false, opConn=false, ipConn=false;
    bool captureReady=false, ok=false;
    if ((*native)->AddCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) cb=true;
    if ((*native)->AddIsochCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) iso=true;
    if ((*native)->TurnOnNotification(native)) notif=true;
    if (capture.allocate()!=kIOReturnSuccess || playback.allocate()!=kIOReturnSuccess) goto cleanup;
    if (macfw::cmp::connectOpcr0(device,opcr0,capture.channel(),capture.speed())!=kIOReturnSuccess) goto cleanup; opConn=true;
    if (macfw::cmp::connectIpcr0(device,ipcr0,playback.channel())!=kIOReturnSuccess) goto cleanup; ipConn=true;
    if ((*playback.nativeChannel())->Start(playback.nativeChannel())!=kIOReturnSuccess) goto cleanup; playStart=true;
    if ((*capture.nativeChannel())->Start(capture.nativeChannel())!=kIOReturnSuccess) goto cleanup; capStart=true;

    {
        std::vector<float> audio(4096 * macfw::hal::kChannels, 0.0f);
        std::vector<std::int32_t> mapped(4096*kPcmChannels,0);
        std::uint64_t lastWrite=input.ring()->writeFrame.load(std::memory_order_acquire);
        std::uint64_t lastCaptureFrames=0;
        CFAbsoluteTime lastStatus=CFAbsoluteTimeGetCurrent();
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

            // Capture is serviced first. Unlike playback, RX DMA continuously
            // reuses a finite receive ring, so completed chunks have a shorter
            // safe observation window. Copy them out before doing Float32
            // playback mapping/refill work.
            capturePump.service(rx, *captureShared.ring());

            // Playback has ~40 ms of scheduler margin (640/320 geometry), so it
            // can safely run after the latency-sensitive RX snapshot.
            drainPlayback(*input.ring(),pcm,audio,mapped);
            UInt32 nowCt=0;
            if ((*native)->GetCycleTime(native,&nowCt)==kIOReturnSuccess)
                streamer.service(cycleCount(nowCt));

            // Service RX again after playback work. The pump's terminal-slot
            // signatures make this idempotent when no new chunk completed,
            // while minimizing the chance that a just-published chunk survives
            // until another loop iteration and is partially reused by DMA.
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
    }
    ok=true;
cleanup:
    if (playStart) (*playback.nativeChannel())->Stop(playback.nativeChannel());
    if (capStart) (*capture.nativeChannel())->Stop(capture.nativeChannel());
    if (ipConn) macfw::cmp::restore(device,macfw::cmp::kIpcr0AddressLo,ipcr0);
    if (opConn) macfw::cmp::restore(device,macfw::cmp::kOpcr0AddressLo,opcr0);
    playback.release(); capture.release();
    if (notif) (*native)->TurnOffNotification(native);
    if (iso) (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (cb) (*native)->RemoveCallbackDispatcherFromRunLoop(native);
    return ok;
}
} // namespace

int halbridge48000_inner_main() {
    gStopRequested = 0;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::cout << "macfw halbridge48000 — native 48 kHz full-duplex CoreAudio HAL to FW410 transport\n";
    return run() ? 0 : 1;
}
