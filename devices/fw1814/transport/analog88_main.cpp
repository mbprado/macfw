#include "../fcp_control.h"
#include "../special_mixer.h"
#include "blocking_pcm_tx88.h"
#include "../tools/fw1814_capture88_pump.h"
#include "duplex_lifecycle.h"
#include "engine_ready.h"
#include "fw1814_control_server.h"

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
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {

constexpr const char* kProduct = "FW 1814";
constexpr unsigned kRate = 88200;
constexpr UInt32 kCaptureMaxPacket = 712;
constexpr UInt32 kPlaybackMaxPacket = 456;
// Keep publication chunks at 32 slots; use a deeper ring for service jitter.
constexpr std::size_t kCaptureSlots = 256;
// The standalone 88.2-kHz probe played clearly with 1280/640 and produced
// broken audio with 640/320. Retain the hardware-validated TX ring here.
constexpr std::size_t kTxPackets = 1280;
constexpr std::size_t kTxHalfPackets = 640;
constexpr std::size_t kPcmCapacityFrames = 262144;
constexpr std::size_t kCapturePrefillFrames = 512;
constexpr UInt32 kCycleLead = 4096;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr std::uint64_t kAudioServicePeriodNs = 250000;
// Start with silent PCM through the duplex rate kick. This initial reserve
// follows the 96-kHz prototype; hardware testing must validate its latency.
constexpr std::size_t kSilentStartupFrames = 8 * kRate / 5;
constexpr std::size_t kMinReadySilenceFrames = 4096;

volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }

bool rollingTxRequested() {
    const char* value = std::getenv("MACFW_88_ROLLING_TX");
    return value && std::strcmp(value, "1") == 0;
}

std::size_t rollingTxLeadPackets() {
    const char* value = std::getenv("MACFW_88_ROLLING_TX_CYCLES");
    if (!value || !*value) return 96;
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    if (!end || *end != '\0' || parsed < 16 || parsed >= kTxPackets)
        return 0;
    return static_cast<std::size_t>(parsed);
}

UInt32 cycleCount(UInt32 cycleTime) {
    return (cycleTime >> 12) & 0x1fffu;
}

bool run() {
    using namespace macfw::fw1814::transport;

    if (access("/tmp/macfw-fw1814-control.sock", F_OK) == 0) {
        std::cerr << "Stop the installed FW1814 supervisor before this experimental engine\n";
        return false;
    }

    SharedPlaybackReader playbackShared;
    if (!playbackShared.open()) {
        std::cerr << "FW1814 playback shared ring unavailable; "
                     "install the guarded 88.2 kHz HAL and open the CoreAudio device\n";
        return false;
    }
    if (playbackShared.ring()->sampleRate.load(std::memory_order_acquire) != kRate) {
        std::cerr << "FW1814 playback shared ring is not set to 88200 Hz\n";
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
    bool rateAttempted = false;
    macfw::fw1814::FcpControl fcp;
    DuplexLifecycle lifecycle;
    Fw1814ControlServer control;
    IsochCallbackRunLoopThread isochCallbackThread;

    if (!macfw::fw1814::applyStraightAnalogPlaybackRouting(device))
        goto cleanup;

    {
        macfw::PcmRingBuffer pcm(kPcmCapacityFrames,
                                 macfw::fw1814::kPlaybackPcmPositions);
        std::vector<std::int32_t> silence(
            kSilentStartupFrames * macfw::fw1814::kPlaybackPcmPositions, 0);
        if (!pcm.valid() || pcm.write(silence.data(), kSilentStartupFrames) !=
                kSilentStartupFrames) {
            std::cerr << "FW1814 88.2 kHz silent warmup preload failed\n";
            goto cleanup;
        }
        auto rx = macfw::AmdtpReceiveRing::create(
            device, kCaptureSlots, kCaptureMaxPacket);
        // The startup PCM preload and RX allocation can take appreciable
        // time. Anchor the 4096-cycle transmit lead only after they finish,
        // as the working standalone tone probe does.
        UInt32 cycleTime = 0;
        if ((*device.nativeHandle())->GetCycleTime(device.nativeHandle(), &cycleTime) !=
            kIOReturnSuccess) {
            std::cerr << "FW1814 GetCycleTime failed\n";
            goto cleanup;
        }
        const UInt32 initialCycle = cycleCount(cycleTime);
        const CFAbsoluteTime cycleAnchorTime = CFAbsoluteTimeGetCurrent();
        const UInt32 firstCycle = (initialCycle + kCycleLead) % kCyclesPerSecond;
        auto tx = macfw::fw1814::transport::BlockingPcmTransmitRing88200::create(
            device, firstCycle, kTxPackets);
        if (!pcm.valid() || !rx || !tx) {
            std::cerr << "FW1814 PCM/ISO ring creation failed\n";
            goto cleanup;
        }

        macfw::fw1814::transport::BlockingPcmStream88200 streamer(
            tx, pcm, initialCycle, firstCycle, kTxHalfPackets);
        const bool rollingTx = rollingTxRequested();
        const std::size_t rollingLead = rollingTx ? rollingTxLeadPackets() : 0;
        const std::size_t rollingGuard = rollingLead / 2;
        if (rollingTx && !streamer.enableRolling(rollingLead, rollingGuard)) {
            std::cerr << "FW1814 invalid 88.2 kHz rolling TX configuration\n";
            goto cleanup;
        }
        if (!streamer.valid() || !streamer.prime()) {
            std::cerr << "FW1814 playback stream prime failed\n";
            goto cleanup;
        }
        std::cout << "FW1814 playback TX ring: " << kTxPackets
                  << " packets / " << kTxHalfPackets
                  << "-packet halves (160 ms / 80 ms)\n";
        if (rollingTx)
            std::cout << "FW1814 experimental 88.2 kHz rolling TX: "
                      << rollingLead << "-cycle live lead, "
                      << rollingGuard << "-cycle deadline guard\n";
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
        if (!fcp.readInputRate(currentRate, false) || currentRate != 48000) {
            std::cerr << "FW1814 authoritative INPUT rate is " << currentRate
                      << " Hz; run make -C devices/fw1814/tools init-48 first\n";
            goto cleanup;
        }

        if (!lifecycle.startIsoch(isochCallbackThread.runLoop())) {
            std::cerr << "FW1814 duplex ISO/CMP start failed\n";
            goto cleanup;
        }
        UInt32 startedCycleTime = 0;
        if ((*device.nativeHandle())->GetCycleTime(device.nativeHandle(),
                                                    &startedCycleTime) != kIOReturnSuccess) {
            std::cerr << "FW1814 GetCycleTime after ISO start failed\n";
            goto cleanup;
        }
        const double isoStartDelayMs =
            (CFAbsoluteTimeGetCurrent() - cycleAnchorTime) * 1000.0;
        std::cout << "FW1814 88.2 kHz TX cycle anchor: " << initialCycle
                  << " first=" << firstCycle
                  << " at-ISO-start=" << cycleCount(startedCycleTime)
                  << " setup-ms=" << isoStartDelayMs << '\n';
        if (isoStartDelayMs >= 450.0) {
            std::cerr << "FW1814 88.2 kHz TX lead nearly expired before audio servicing; "
                         "refusing rate kick\n";
            goto cleanup;
        }
        isochCallbackThread.startPumping();

        std::cout << "FW1814 duplex ISO started: playback ch="
                  << lifecycle.playbackChannel()
                  << " capture ch=" << lifecycle.captureChannel() << '\n';
        std::cout << "FW1814 isoch callback dispatcher: dedicated run-loop thread\n";
        // Start TX servicing before the special FCP kick. The FCP response
        // loop and 100 ms INPUT delay must not starve the 80 ms TX halves.
        std::atomic<bool> rateKicked{false};
        std::atomic<bool> releasePlayback{false};

        macfw::fw1814::experimental::CapturePump88200 capturePump;
        PlaybackPumpStats playbackPumpStats;
        std::vector<float> audio(
            4096 * macfw::fw1814::hal::kOutputChannels, 0.0f);
        std::vector<std::int32_t> mapped(
            4096 * macfw::fw1814::kPlaybackPcmPositions, 0);

        std::cout << "FW1814 experimental 88.2 kHz analog engine starting\n"
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
            AudioLoopTimingStats loopTiming;

            while (!gStopRequested) {
                const std::uint64_t wakeLateTicks = pacer.wait();
                const std::uint64_t loopStartTicks =
                    verbose ? mach_absolute_time() : 0;

                if (rateKicked.load(std::memory_order_acquire))
                    capturePump.service(rx, *captureShared.ring());
                const std::uint64_t firstCaptureDoneTicks =
                    verbose ? mach_absolute_time() : 0;
                if (releasePlayback.load(std::memory_order_acquire))
                    drainPlayback(*playbackShared.ring(), pcm, audio, mapped,
                                  &playbackPumpStats);
                const std::uint64_t playbackDoneTicks =
                    verbose ? mach_absolute_time() : 0;

                UInt32 nowCycleTime = 0;
                if ((*device.nativeHandle())->GetCycleTime(
                        device.nativeHandle(), &nowCycleTime) == kIOReturnSuccess)
                    streamer.service(cycleCount(nowCycleTime));
                if (!streamer.healthy()) {
                    const auto& txStats = streamer.stats();
                    std::cerr << "FW1814 88.2 kHz rolling TX deadline missed; "
                                 "stopping before unsafe slot reuse (misses="
                              << txStats.rollingDeadlineMisses
                              << ", max-cycle-gap=" << txStats.maxCycleDelta
                              << ")\n";
                    audioFinished.store(true, std::memory_order_release);
                    return;
                }
                const std::uint64_t txDoneTicks =
                    verbose ? mach_absolute_time() : 0;

                if (rateKicked.load(std::memory_order_acquire))
                    capturePump.service(rx, *captureShared.ring());
                const std::uint64_t secondCaptureDoneTicks =
                    verbose ? mach_absolute_time() : 0;
                if (verbose) {
                    loopTiming.observe(
                        wakeLateTicks,
                        (firstCaptureDoneTicks - loopStartTicks) +
                            (secondCaptureDoneTicks - txDoneTicks),
                        playbackDoneTicks - firstCaptureDoneTicks,
                        txDoneTicks - playbackDoneTicks,
                        secondCaptureDoneTicks - loopStartTicks);
                }


                if (rateKicked.load(std::memory_order_acquire) &&
                    captureShared.ring()->active.load(std::memory_order_acquire) == 0) {
                    const bool resumed = captureReady;
                    captureReady = false;
                    if (captureShared.activateForConsumer(kCapturePrefillFrames)) {
                        captureReady = true;
                        std::cout << (resumed ? "FW1814 capture consumer resumed; "
                                              "fresh capture enabled\n"
                                              : "FW1814 capture consumer detected; "
                                                "live capture enabled\n");
                    }
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
                              << " tx-nonzero=" << txStats.nonzeroFrames
                              << " marker-pb=" << playbackPumpStats.firstLoudHostTime
                              << " marker-tx=" << txStats.firstLoudHostTime
                              << " marker-cap=" << rxStats.firstLoudHostTime
                              << " tx-peak=" << txStats.peakSample
                              << " tx-late=" << txStats.lateCyclePolls
                              << " tx-max-gap=" << txStats.maxCycleDelta
                              << " tx-roll-packets=" << txStats.rollingPacketsRefilled
                              << " tx-roll-miss=" << txStats.rollingDeadlineMisses
                              << " pcm-underrun=" << pcm.underrunFrames()
                              << " pb-read=" << playbackPumpStats.framesRead
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
                              << " cap-drop=" << captureShared.ring()->droppedFrames.load(std::memory_order_relaxed)
                              << " cap-active=" << captureShared.ring()->active.load(std::memory_order_relaxed)
                              << " hal-in-reads=" << captureShared.ring()->halReadCalls.load(std::memory_order_relaxed)
                              << " hal-in-frames=" << captureShared.ring()->halFramesFromRing.load(std::memory_order_relaxed)
                              << " hal-in-zero=" << captureShared.ring()->halZeroFilledFrames.load(std::memory_order_relaxed)
                              << " hal-in-underrun=" << captureShared.ring()->halUnderrunEvents.load(std::memory_order_relaxed)
                              << " rx-touched=" << rx.touchedCount() << '/' << rx.packetCount()
                              << " chunks=" << rxStats.completedChunks
                              << " malformed=" << captureShared.ring()->malformedPackets.load()
                              << " invalid=" << captureShared.ring()->invalidLabels.load()
                              << " nodata=" << rxStats.noDataPackets
                              << " dbc-gap=" << rxStats.dbcDiscontinuities
                              << " duplicates=" << rxStats.duplicateSlots
                              << " incomplete=" << rxStats.incompleteGroups
                              << " recovered=" << rxStats.recoveredGroups
                              << " overwritten=" << rxStats.overwrittenGroups
                              << " salvaged=" << rxStats.salvagedGroups
                              << " skipped-slots=" << rxStats.skippedSlots
                              << " first-incomplete-slot=" << rxStats.firstIncompleteSlot
                              << " ts-regress=" << rxStats.timestampRegressions
                              << " metadata-swaps=" << rxStats.metadataByteSwaps
                              << " reorder=" << rxStats.reorderedPackets
                              << " stale=" << rxStats.stalePackets;
                    loopTiming.append(std::cout);
                    std::cout << '\n';
                    loopTiming.reset();
                    lastCaptureFrames = captureFrames;
                    lastStatus = now;
                }
            }

            audioOk = true;
            audioFinished.store(true, std::memory_order_release);
        });

        // The dedicated audio thread services TX throughout both CONTROL
        // transactions and the 1-second post-kick silent warmup.
        bool startupOk = true;
        std::cout << "FW1814 88.2 kHz TX lead: 4096 cycles; waiting 550 ms\n";
        const CFAbsoluteTime firstCycleDeadline = CFAbsoluteTimeGetCurrent() + 0.550;
        while (CFAbsoluteTimeGetCurrent() < firstCycleDeadline && !gStopRequested &&
               !audioFinished.load(std::memory_order_acquire))
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
        if (gStopRequested || audioFinished.load(std::memory_order_acquire))
            startupOk = false;
        if (startupOk) {
            rateAttempted = true;
            std::cout << "FW1814 special stream kick: OUTPUT 88200 Hz\n";
            startupOk = fcp.setSignalRate(kRate, 0x18, false);
        }
        if (startupOk) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << "FW1814 special stream kick: INPUT 88200 Hz\n";
            startupOk = fcp.setSignalRate(kRate, 0x19, false);
        }
        if (startupOk) {
            // In the standalone probe capture remained mostly NODATA during
            // the first half-second after INPUT CONTROL. Withhold it from HAL.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            rateKicked.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            startupOk = !gStopRequested &&
                !audioFinished.load(std::memory_order_acquire) &&
                lifecycle.generationStillValid();
        }
        if (startupOk) {
            if (!control.start(device, kRate))
                std::cerr << "warning: FW1814 control socket unavailable\n";
            // Only the audio thread reads this ring. Before releasing its HAL
            // producer, top up a short silence reserve if a slow FCP exchange
            // consumed more of the startup preload than expected.
            const auto queued = pcm.availableFrames();
            if (queued < kMinReadySilenceFrames &&
                pcm.write(silence.data(), kMinReadySilenceFrames - queued) !=
                    kMinReadySilenceFrames - queued) {
                std::cerr << "FW1814 88.2 kHz ready silence top-up failed\n";
                startupOk = false;
            }
            std::cout << "FW1814 88.2 kHz playback reserve at READY: "
                      << pcm.availableFrames() << " frames ("
                      << pcm.availableFrames() * 1000 / kRate << " ms); "
                      << "pre-ready TX underrun=" << pcm.underrunFrames()
                      << " frames\n";
        }
        if (startupOk) {
            releasePlayback.store(true, std::memory_order_release);
            signalEngineReady();
            playbackShared.ring()->active.store(1, std::memory_order_release);
            playbackActive = true;
            std::cout << "FW1814 88.2 kHz analog engine ONLINE\n";
        }

        // Keep the FireWire general callback dispatcher alive independently of
        // the realtime audio service. This is the same scheduling separation
        // used by the released FW410 runtime.
        while (startupOk && !gStopRequested &&
               !audioFinished.load(std::memory_order_acquire)) {
            control.service();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
        }

        gStopRequested = 1;
        audioThread.join();
        ok = startupOk && audioOk;
    }

cleanup:
    control.reset();
    if (playbackActive)
        playbackShared.ring()->active.store(0, std::memory_order_release);

    const bool restoreOk = lifecycle.stopIsochAndRestoreCmp();
    if (rateAttempted && lifecycle.generationStillValid()) {
        std::cout << "FW1814 experimental 88.2 kHz: restoring 48 kHz baseline\n";
        const bool outputOk = fcp.setSignalRate(48000, 0x18, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const bool inputOk = fcp.setSignalRate(48000, 0x19, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        unsigned restoredRate = 0;
        const bool rateOk = fcp.readInputRate(restoredRate, false) &&
                            restoredRate == 48000;
        std::cout << "FW1814 48 kHz restore: "
                  << (outputOk && inputOk && rateOk ? "PASS" : "FAIL") << '\n';
        if (!outputOk || !inputOk || !rateOk) ok = false;
    } else if (rateAttempted) {
        std::cerr << "FW1814 generation changed; skip stale rate restore\n";
        ok = false;
    }
    fcp.reset();
    lifecycle.removeDispatchers();
    isochCallbackThread.stop();
    lifecycle.stop();

    if (!restoreOk && ok) ok = false;
    device.close();
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    const bool legacyExperimental =
        argc == 2 && std::strcmp(argv[1], "--experimental-high-rate") == 0;
    if (argc != 1 && !legacyExperimental) {
        std::cerr << "usage: fw1814analog88\n";
        return 64;
    }
    gStopRequested = 0;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    // Flush diagnostics immediately during this manual guarded prototype.
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    std::cout << "macfw fw1814analog88 — 88.2 kHz analog full-duplex engine\n";
    return run() ? 0 : 1;
}
