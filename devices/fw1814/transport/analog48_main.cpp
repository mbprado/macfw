#include "../fcp_control.h"
#include "../special_mixer.h"
#include "blocking_pcm_tx.h"
#include "capture_pump.h"
#include "duplex_lifecycle.h"
#include "engine_ready.h"
#include "fw1814_control_server.h"
#include "pcm_stream48.h"
#include "playback_pump.h"
#include "realtime_service.h"
#include "shared_io.h"

#include "macfw/amdtp_receive_ring.h"
#include "macfw/firewire_device.h"
#include "macfw/pcm_ring_buffer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {

constexpr const char* kProduct = "FW 1814";
constexpr unsigned kRate = 48000;
constexpr UInt32 kCaptureMaxPacket = 360;
constexpr UInt32 kPlaybackMaxPacket = 232;
// Use the same production receive depth as the released FW410 path. The early
// FW1814 bring-up temporarily used 64 slots only to match duplex-blocking-raw;
// the later DBS=11 event-width fix resolved the actual capture decode problem.
// A 64-slot cyclic ring can expose a slot-63 -> slot-0 publication race after
// some reconnect phases, observed as one missing 8-frame data packet per ring
// revolution (47 kHz decoded instead of 48 kHz).
constexpr std::size_t kCaptureSlots = 256;
// Hardware-validated dynamic playback geometry. Do not reduce without
// arbitrary-frequency and real-audio regression testing.
constexpr std::size_t kTxPackets = 640;
constexpr std::size_t kTxHalfPackets = 320;
constexpr std::size_t kPcmCapacityFrames = 16384;
constexpr std::size_t kCapturePrefillFrames = 512;
constexpr UInt32 kCycleLead = 256;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr std::uint64_t kAudioServicePeriodNs = 250000;

volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }

UInt32 cycleCount(UInt32 cycleTime) {
    return (cycleTime >> 12) & 0x1fffu;
}

bool run() {
    using namespace macfw::fw1814::transport;

    SharedPlaybackReader playbackShared;
    if (!playbackShared.open()) {
        std::cerr << "FW1814 playback shared ring unavailable. "
                     "Run fw1814shmtest --init first (HAL will own this later).\n";
        return false;
    }
    if (playbackShared.ring()->sampleRate.load(std::memory_order_acquire) != kRate) {
        std::cerr << "FW1814 playback shared ring is not set to 48000 Hz\n";
        return false;
    }
    playbackShared.discardBacklog();

    SharedCaptureWriter captureShared;
    if (!captureShared.open(kRate))
        return false;

    auto device = macfw::FireWireDevice::findByProductName(kProduct);
    if (!device) {
        std::cerr << "No operational FW 1814 unit found\n";
        return false;
    }
    if (device.open() != kIOReturnSuccess) {
        std::cerr << "FW1814 open failed\n";
        return false;
    }

    bool ok = false;
    bool playbackActive = false;
    macfw::fw1814::FcpControl fcp;
    DuplexLifecycle lifecycle;
    Fw1814ControlServer control;
    IsochCallbackRunLoopThread isochCallbackThread;

    if (!macfw::fw1814::applyStraightAnalogPlaybackRouting(device))
        goto cleanup;

    {
        UInt32 cycleTime = 0;
        if ((*device.nativeHandle())->GetCycleTime(device.nativeHandle(), &cycleTime) !=
            kIOReturnSuccess) {
            std::cerr << "FW1814 GetCycleTime failed\n";
            goto cleanup;
        }
        const UInt32 initialCycle = cycleCount(cycleTime);
        const UInt32 firstCycle = (initialCycle + kCycleLead) % kCyclesPerSecond;

        macfw::PcmRingBuffer pcm(kPcmCapacityFrames,
                                 macfw::fw1814::kPlaybackPcmPositions);
        auto rx = macfw::AmdtpReceiveRing::create(
            device, kCaptureSlots, kCaptureMaxPacket);
        auto tx = BlockingPcmTransmitRing48k::create(
            device, firstCycle, kTxPackets);
        if (!pcm.valid() || !rx || !tx) {
            std::cerr << "FW1814 PCM/ISO ring creation failed\n";
            goto cleanup;
        }

        BlockingPcmStream48k streamer(
            tx, pcm, initialCycle, firstCycle, kTxHalfPackets);
        if (!streamer.valid() || !streamer.prime()) {
            std::cerr << "FW1814 playback stream prime failed\n";
            goto cleanup;
        }
        std::cout << "FW1814 playback TX ring: " << kTxPackets
                  << " packets / " << kTxHalfPackets
                  << "-packet halves (80 ms / 40 ms)\n";
        std::cout << "FW1814 capture RX ring: " << kCaptureSlots
                  << " packets / 32-packet publication chunks\n";

        if (!lifecycle.prepare(device, rx, tx.nativeLocalPort(),
                               kCaptureMaxPacket, kPlaybackMaxPacket)) {
            std::cerr << "FW1814 duplex lifecycle prepare failed\n";
            goto cleanup;
        }
        // FCP/general callbacks stay on the engine main thread. Isoch callbacks
        // move to their own user-interactive run-loop thread below.
        if (!lifecycle.addCallbackDispatcher()) {
            std::cerr << "FW1814 callback dispatcher setup failed\n";
            goto cleanup;
        }
        if (!fcp.arm(device)) {
            std::cerr << "FW1814 FCP response handler setup failed\n";
            goto cleanup;
        }
        if (!isochCallbackThread.prepare()) {
            std::cerr << "FW1814 dedicated isoch callback thread setup failed\n";
            goto cleanup;
        }

        unsigned currentRate = 0;
        if (!fcp.readInputRate(currentRate, false) || currentRate != kRate) {
            std::cerr << "FW1814 authoritative INPUT rate is " << currentRate
                      << " Hz; run make -C devices/fw1814/tools init-48 first\n";
            goto cleanup;
        }

        if (!lifecycle.startIsoch(isochCallbackThread.runLoop())) {
            std::cerr << "FW1814 duplex ISO/CMP start failed\n";
            goto cleanup;
        }
        isochCallbackThread.startPumping();

        std::cout << "FW1814 duplex ISO started: playback ch="
                  << lifecycle.playbackChannel()
                  << " capture ch=" << lifecycle.captureChannel() << '\n';
        std::cout << "FW1814 isoch callback dispatcher: dedicated run-loop thread\n";
        std::cout << "FW1814 ISO settle: 50 ms\n";
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.050, false);

        std::cout << "FW1814 special stream kick: OUTPUT 48000 Hz\n";
        if (!fcp.setSignalRate(kRate, 0x18, false)) {
            std::cerr << "FW1814 OUTPUT rate CONTROL failed\n";
            goto cleanup;
        }

        std::cout << "FW1814 special stream kick: wait 100 ms\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::cout << "FW1814 special stream kick: INPUT 48000 Hz\n";
        if (!fcp.setSignalRate(kRate, 0x19, false)) {
            std::cerr << "FW1814 INPUT rate CONTROL failed\n";
            goto cleanup;
        }

        std::cout << "FW1814 stream kick: PASS"
                  << " (matched INTERIMs=" << fcp.matchedInterimCount()
                  << ", ignored unrelated=" << fcp.ignoredResponseCount() << ")\n";

        if (!control.start(device, kRate))
            std::cerr << "warning: FW1814 control socket unavailable; audio will continue\n";

        signalEngineReady();

        playbackShared.ring()->active.store(1, std::memory_order_release);
        playbackActive = true;

        CapturePump48k capturePump;
        PlaybackPumpStats playbackPumpStats;
        std::vector<float> audio(
            4096 * macfw::fw1814::hal::kOutputChannels, 0.0f);
        std::vector<std::int32_t> mapped(
            4096 * macfw::fw1814::kPlaybackPcmPositions, 0);

        std::cout << "FW1814 analog engine ONLINE\n"
                  << "    CoreAudio-facing outputs: Analog 1-4\n"
                  << "    CoreAudio-facing inputs:  Analog 1-8\n"
                  << "    digital/MIDI/headphone levels: deferred\n"
                  << "    audio service: dedicated Mach-paced thread (250 us)\n"
                  << "    Ctrl-C to stop\n";

        std::atomic<bool> audioFinished{false};
        bool audioOk = false;

        std::thread audioThread([&] {
            requestInteractiveQos("FW1814 audio service thread");
            requestAudioTimeConstraint();
            MachPacer pacer(kAudioServicePeriodNs);
            if (!pacer.valid()) {
                std::cerr << "FW1814 Mach pacing setup failed\n";
                audioFinished.store(true, std::memory_order_release);
                return;
            }

            bool captureReady = false;
            const bool verbose = std::getenv("MACFW_VERBOSE") != nullptr;
            CFAbsoluteTime lastGenerationCheck = CFAbsoluteTimeGetCurrent();
            CFAbsoluteTime lastStatus = lastGenerationCheck;
            std::uint64_t lastCaptureFrames = 0;

            while (!gStopRequested) {
                pacer.wait();

                capturePump.service(rx, *captureShared.ring());
                drainPlayback(*playbackShared.ring(), pcm, audio, mapped,
                              &playbackPumpStats);

                UInt32 nowCycleTime = 0;
                if ((*device.nativeHandle())->GetCycleTime(
                        device.nativeHandle(), &nowCycleTime) == kIOReturnSuccess)
                    streamer.service(cycleCount(nowCycleTime));

                capturePump.service(rx, *captureShared.ring());

                if (!captureReady &&
                    captureShared.activateForConsumer(kCapturePrefillFrames)) {
                    captureReady = true;
                    std::cout << "FW1814 capture consumer detected; live capture enabled\n";
                }

                const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
                if (now - lastGenerationCheck >= 0.25) {
                    if (!lifecycle.generationStillValid()) {
                        std::cerr << "FW1814 FireWire generation changed during streaming; "
                                     "requesting engine restart\n";
                        audioFinished.store(true, std::memory_order_release);
                        return;
                    }
                    lastGenerationCheck = now;
                }

                if (verbose && now - lastStatus >= 2.0) {
                    const auto captureFrames =
                        captureShared.ring()->decodedFrames.load(std::memory_order_acquire);
                    const auto& txStats = streamer.stats();
                    const auto& rxStats = capturePump.stats();
                    const auto* pb = playbackShared.ring();
                    std::cout << "FW1814 out-shared="
                              << macfw::fw1814::hal::availableFrames(*pb)
                              << " pcm=" << pcm.availableFrames()
                              << " tx-audio=" << txStats.framesFromBuffer
                              << " tx-silence=" << txStats.framesSilenced
                              << " tx-late=" << txStats.lateCyclePolls
                              << " hal-calls=" << pb->doIOCalls.load(std::memory_order_relaxed)
                              << " hal-frames=" << pb->doIOFrames.load(std::memory_order_relaxed)
                              << " hal-drop=" << pb->droppedFrames.load(std::memory_order_relaxed)
                              << " in-peak=" << playbackPumpStats.peakAbs
                              << " clip=" << playbackPumpStats.clippedSamples
                              << " nonfinite=" << playbackPumpStats.nonFiniteSamples
                              << " | capture=" << captureFrames
                              << " (delta " << (captureFrames - lastCaptureFrames) << ')'
                              << " queued="
                              << macfw::fw1814::hal::capture::availableFrames(*captureShared.ring())
                              << " rx-touched=" << rx.touchedCount() << '/' << rx.packetCount()
                              << " chunks=" << rxStats.completedChunks
                              << " malformed=" << captureShared.ring()->malformedPackets.load()
                              << " invalid=" << captureShared.ring()->invalidLabels.load()
                              << " nodata=" << rxStats.noDataPackets
                              << " dbc-gap=" << rxStats.dbcDiscontinuities
                              << " reorder=" << rxStats.reorderedPackets
                              << " stale=" << rxStats.stalePackets << '\n';
                    lastCaptureFrames = captureFrames;
                    lastStatus = now;
                }
            }

            audioOk = true;
            audioFinished.store(true, std::memory_order_release);
        });

        // Keep the FireWire general callback dispatcher alive independently of
        // the realtime audio service. This is the same scheduling separation
        // used by the released FW410 runtime.
        while (!gStopRequested && !audioFinished.load(std::memory_order_acquire)) {
            control.service();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
        }

        audioThread.join();
        ok = audioOk;
    }

cleanup:
    control.reset();
    if (playbackActive)
        playbackShared.ring()->active.store(0, std::memory_order_release);

    const bool restoreOk = lifecycle.stopIsochAndRestoreCmp();
    fcp.reset();
    lifecycle.removeDispatchers();
    isochCallbackThread.stop();
    lifecycle.stop();

    if (!restoreOk && ok) ok = false;
    device.close();
    return ok;
}

} // namespace

int main() {
    gStopRequested = 0;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    // launchd redirects stdout/stderr to regular files, so C++ iostreams are
    // not terminal-line-buffered. Flush every insertion while this runtime is
    // still in diagnostic bring-up so scheduling/recovery failures are visible
    // immediately in /Library/Logs/macfw-fw1814-transport.log.
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    std::cout << "macfw fw1814analog48 — experimental 48 kHz analog full-duplex engine\n";
    return run() ? 0 : 1;
}
