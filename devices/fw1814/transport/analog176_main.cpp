#include "../fcp_control.h"
#include "../special_mixer.h"
#include "blocking_pcm_tx176.h"
#include "../tools/fw1814_capture176_pump.h"
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
constexpr unsigned kRate = 176400;
constexpr UInt32 kCaptureMaxPacket = 392;
constexpr UInt32 kPlaybackMaxPacket = 648;
// Keep publication chunks at 32 slots; use a deeper ring for service jitter.
constexpr std::size_t kCaptureSlots = 256;
// The standalone 176.4-kHz probe played clearly with 1280/640 and produced
// broken audio with 640/320. Retain the hardware-validated TX ring here.
constexpr std::size_t kTxPackets = 1280;
constexpr std::size_t kTxHalfPackets = 640;
// At 176.4 kHz each 640-cycle half contains 441 data packets with 32 PCM
// frames each. A clean hardware transition retained four complete halves of
// silence at READY; broken transitions retained only two.
constexpr std::size_t kFramesPerTxHalf = 441 * 32;
constexpr std::size_t kPcmCapacityFrames = 524288;
constexpr std::size_t kCapturePrefillFrames = 512;
constexpr std::size_t kQuadPlaybackPcmPositions = 4;
constexpr UInt32 kCycleLead = 4096;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr std::uint64_t kAudioServicePeriodNs = 250000;
// Start with silent PCM through the duplex rate kick. This initial reserve
// follows the 96-kHz prototype; hardware testing must validate its latency.
constexpr std::size_t kSilentStartupFrames = 8 * kRate / 5;
constexpr std::size_t kReadySilenceFrames = 4 * kFramesPerTxHalf;
// Match the roughly 43-46 ms live-release reserve used by the stable
// 88.2/96 kHz engines without changing the validated 1280/640 TX geometry.
constexpr std::size_t kReleaseSilenceFrames = 8192;
constexpr std::chrono::milliseconds kHalPlaybackPrearmTimeout(1000);

volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }

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
                     "install the guarded 176.4 kHz HAL and open the CoreAudio device\n";
        return false;
    }
    if (playbackShared.ring()->sampleRate.load(std::memory_order_acquire) != kRate) {
        std::cerr << "FW1814 playback shared ring is not set to 176400 Hz\n";
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
                                 kQuadPlaybackPcmPositions);
        std::vector<std::int32_t> silence(
            kSilentStartupFrames * kQuadPlaybackPcmPositions, 0);
        if (!pcm.valid() || pcm.write(silence.data(), kSilentStartupFrames) !=
                kSilentStartupFrames) {
            std::cerr << "FW1814 176.4 kHz silent warmup preload failed\n";
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
        auto tx = macfw::fw1814::transport::BlockingPcmTransmitRing176400::create(
            device, firstCycle, kTxPackets);
        if (!pcm.valid() || !rx || !tx) {
            std::cerr << "FW1814 PCM/ISO ring creation failed\n";
            goto cleanup;
        }

        macfw::fw1814::transport::BlockingPcmStream176400 streamer(
            tx, pcm, initialCycle, firstCycle, kTxHalfPackets);
        if (!streamer.valid() || !streamer.prime()) {
            std::cerr << "FW1814 playback stream prime failed\n";
            goto cleanup;
        }
        std::cout << "FW1814 playback TX ring: " << kTxPackets
                  << " packets / " << kTxHalfPackets
                  << "-packet halves (160 ms / 80 ms)\n";
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
        std::cout << "FW1814 176.4 kHz TX cycle anchor: " << initialCycle
                  << " first=" << firstCycle
                  << " at-ISO-start=" << cycleCount(startedCycleTime)
                  << " setup-ms=" << isoStartDelayMs << '\n';
        if (isoStartDelayMs >= 450.0) {
            std::cerr << "FW1814 176.4 kHz TX lead nearly expired before audio servicing; "
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
        std::atomic<std::int64_t> playbackReleaseNs{0};

        macfw::fw1814::experimental::CapturePump176400 capturePump;
        PlaybackPumpStats playbackPumpStats;
        std::vector<float> audio(
            4096 * macfw::fw1814::hal::kOutputChannels, 0.0f);
        std::vector<std::int32_t> mapped(
            4096 * kQuadPlaybackPcmPositions, 0);

        std::cout << "FW1814 experimental 176.4 kHz analog engine starting\n"
                  << "    CoreAudio-facing outputs: Analog 1-4\n"
                  << "    CoreAudio-facing inputs:  Analog 1-2\n"
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
            bool firstSharedReadLogged = false;
            bool firstNonzeroTxLogged = false;
            const bool verbose = std::getenv("MACFW_VERBOSE") != nullptr;
            CFAbsoluteTime lastGenerationCheck = CFAbsoluteTimeGetCurrent();
            CFAbsoluteTime lastStatus = lastGenerationCheck;
            std::uint64_t lastCaptureFrames = 0;

            while (!gStopRequested) {
                pacer.wait();

                if (rateKicked.load(std::memory_order_acquire))
                    capturePump.service(rx, *captureShared.ring());
                if (releasePlayback.load(std::memory_order_acquire)) {
                    const auto framesBefore = playbackPumpStats.framesRead;
                    drainPlaybackQuad(*playbackShared.ring(), pcm, audio, mapped,
                                      &playbackPumpStats);
                    if (!firstSharedReadLogged &&
                        playbackPumpStats.framesRead != framesBefore) {
                        const auto released =
                            playbackReleaseNs.load(std::memory_order_acquire);
                        const auto now = std::chrono::duration_cast<
                            std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                                             .count();
                        std::cout << "FW1814 176.4 kHz first shared playback read: "
                                  << (now - released) / 1000000
                                  << " ms after release\n";
                        firstSharedReadLogged = true;
                    }
                }

                UInt32 nowCycleTime = 0;
                if ((*device.nativeHandle())->GetCycleTime(
                        device.nativeHandle(), &nowCycleTime) == kIOReturnSuccess)
                    streamer.service(cycleCount(nowCycleTime));

                if (releasePlayback.load(std::memory_order_acquire) &&
                    !firstNonzeroTxLogged &&
                    streamer.stats().nonzeroFrames != 0) {
                    const auto released =
                        playbackReleaseNs.load(std::memory_order_acquire);
                    const auto now = std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                                         .count();
                    std::cout << "FW1814 176.4 kHz first nonzero TX: "
                              << (now - released) / 1000000
                              << " ms after release\n";
                    firstNonzeroTxLogged = true;
                }

                if (rateKicked.load(std::memory_order_acquire))
                    capturePump.service(rx, *captureShared.ring());

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
                              << " tx-peak=" << txStats.peakSample
                              << " tx-late=" << txStats.lateCyclePolls
                              << " tx-danger=" << txStats.dangerousCyclePolls
                              << " tx-max-gap=" << txStats.maxCycleDelta
                              << " tx-max-behind=" << txStats.maxHalvesBehind
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
                              << " stale=" << rxStats.stalePackets << '\n';
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
        std::cout << "FW1814 176.4 kHz TX lead: 4096 cycles; waiting 550 ms\n";
        const CFAbsoluteTime firstCycleDeadline = CFAbsoluteTimeGetCurrent() + 0.550;
        while (CFAbsoluteTimeGetCurrent() < firstCycleDeadline && !gStopRequested &&
               !audioFinished.load(std::memory_order_acquire))
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
        if (gStopRequested || audioFinished.load(std::memory_order_acquire))
            startupOk = false;
        if (startupOk) {
            rateAttempted = true;
            std::cout << "FW1814 special stream kick: OUTPUT 176400 Hz\n";
            startupOk = fcp.setSignalRate(kRate, 0x18, false);
        }
        if (startupOk) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << "FW1814 special stream kick: INPUT 176400 Hz\n";
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
            // Only the audio thread reads this ring. Establish the guarded
            // reserve before notifying the supervisor that transport is ready.
            const auto queued = pcm.availableFrames();
            if (queued < kReadySilenceFrames) {
                const auto topUpFrames = kReadySilenceFrames - queued;
                if (pcm.write(silence.data(), topUpFrames) != topUpFrames) {
                    std::cerr << "FW1814 176.4 kHz ready silence top-up failed\n";
                    startupOk = false;
                } else {
                    std::cout << "FW1814 176.4 kHz ready silence top-up: "
                              << topUpFrames << " frames\n";
                }
            }
            std::cout << "FW1814 176.4 kHz playback reserve at READY: "
                      << pcm.availableFrames() << " frames ("
                      << pcm.availableFrames() * 1000 / kRate << " ms); "
                      << "pre-ready TX underrun=" << pcm.underrunFrames()
                      << " frames\n";
        }
        if (startupOk) {
            signalEngineReady();
            // The supervisor now restores the saved mixer state through the
            // control socket. Keep transmitting silence until its final
            // CONTROL READY handshake so live CoreAudio cannot overlap the
            // register-write sequence at quad rate.
            while (!control.controlStateReady() && !gStopRequested &&
                   !audioFinished.load(std::memory_order_acquire)) {
                control.service();
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
            }
            startupOk = !gStopRequested &&
                !audioFinished.load(std::memory_order_acquire) &&
                lifecycle.generationStillValid();
        }
        if (startupOk) {
            // Arm the HAL before releasing the playback pump. A full firmware
            // reboot can make CoreAudio resume later than the transport, so
            // wait for its first fresh callback instead of consuming the
            // entire PCM safety reserve while the shared ring is still empty.
            playbackShared.discardBacklog();
            auto* playbackRing = playbackShared.ring();
            const auto prearmStart = std::chrono::steady_clock::now();
            playbackRing->active.store(1, std::memory_order_release);
            playbackActive = true;
            while (macfw::fw1814::hal::availableFrames(*playbackRing) == 0 &&
                   !gStopRequested &&
                   !audioFinished.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() - prearmStart <
                       kHalPlaybackPrearmTimeout) {
                control.service();
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
            }
            const auto prearmMs = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - prearmStart).count();
            const auto prearmFrames =
                macfw::fw1814::hal::availableFrames(*playbackRing);
            if (prearmFrames != 0) {
                std::cout << "FW1814 176.4 kHz HAL playback pre-arm: "
                          << prearmFrames << " frames after " << prearmMs
                          << " ms\n";
            } else {
                std::cout << "FW1814 176.4 kHz HAL playback pre-arm: "
                          << "no active client after " << prearmMs
                          << " ms; continuing guarded release\n";
            }
            startupOk = !gStopRequested &&
                !audioFinished.load(std::memory_order_acquire) &&
                lifecycle.generationStillValid();
        }
        if (startupOk) {
            const auto queued = pcm.availableFrames();
            if (queued < kReleaseSilenceFrames) {
                const auto topUpFrames = kReleaseSilenceFrames - queued;
                if (pcm.write(silence.data(), topUpFrames) != topUpFrames) {
                    std::cerr << "FW1814 176.4 kHz playback-release silence top-up failed\n";
                    startupOk = false;
                } else {
                    std::cout << "FW1814 176.4 kHz playback-release silence top-up: "
                              << topUpFrames << " frames\n";
                }
            }
        }
        if (startupOk) {
            std::cout << "FW1814 176.4 kHz playback release reserve: "
                      << pcm.availableFrames() << " frames ("
                      << pcm.availableFrames() * 1000 / kRate << " ms)\n";
            const auto releaseNs = std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            playbackReleaseNs.store(releaseNs, std::memory_order_release);
            releasePlayback.store(true, std::memory_order_release);
            std::cout << "FW1814 176.4 kHz analog engine ONLINE\n";
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
        std::cout << "FW1814 experimental 176.4 kHz: restoring 48 kHz baseline\n";
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
    if (argc != 3 ||
        std::strcmp(argv[1], "--experimental-high-rate") != 0 ||
        std::strcmp(argv[2], "--experimental-quad-rate") != 0) {
        std::cerr << "usage: fw1814analog176 --experimental-high-rate --experimental-quad-rate\n"
                     "This guarded engine requires a 176.4 kHz playback SHM "
                     "and stopped FW1814 supervisor.\n";
        return 64;
    }
    gStopRequested = 0;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    // Flush diagnostics immediately during this manual guarded prototype.
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    std::cout << "macfw fw1814analog176 — experimental 176.4 kHz analog full-duplex engine\n";
    return run() ? 0 : 1;
}
