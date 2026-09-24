#include "../fcp_control.h"
#include "../special_mixer.h"
#include "blocking_pcm_tx44.h"
#include "capture_pump44.h"
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

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

namespace {

constexpr const char* kProduct = "FW 1814";
constexpr unsigned kRate = 44100;
constexpr UInt32 kCaptureMaxPacket = 360;
constexpr UInt32 kPlaybackMaxPacket = 232;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kTxPackets = 640;
constexpr std::size_t kTxHalfPackets = 320;
constexpr std::size_t kRollingTxDefaultLeadPackets = 96;
// Match the hardware-clean native 44.1 tone probe for this diagnostic.  Its
// preloaded 262144-frame PCM ring removes concurrent small-ring pressure from
// the comparison, leaving the SHM float conversion as the only added stage.
constexpr std::size_t kPcmCapacityFrames = 262144;
constexpr std::size_t kCapturePrefillFrames = 512;
// Keep the previously validated startup geometry available for the fallback
// path. The rolling path starts closer to the live bus cursor, like the 48 kHz
// engine and Linux's continuously recycled AMDTP queue.
constexpr UInt32 kValidatedCycleLead = 2048;
constexpr UInt32 kRollingCycleLead = 256;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr std::uint64_t kAudioServicePeriodNs = 250000;
constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kPrimePcmFrames = 441 * 8;
constexpr std::size_t kWarmupPcmFrames = 2 * kRate;
// Retain the hardware-proven ISO startup preload, but let its unused silence
// drain before admitting live HAL frames. The normal TX/audio service loop
// runs throughout; this only changes when the playback SHM is consumed.
// One 320-packet TX refill consumes about 1764 frames at 44.1 kHz. Allow
// live playback into PCM after the silent preload has fallen to roughly one
// refill, instead of retaining ~170 ms of silence throughout the session.
constexpr std::size_t kLivePcmReserveFrames = 2048;
constexpr std::size_t kRollingLivePcmReserveFrames = 512;

volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }

UInt32 cycleCount(UInt32 cycleTime) {
    return (cycleTime >> 12) & 0x1fffu;
}

UInt32 cycleDelta(UInt32 newer, UInt32 older) {
    return (newer + kCyclesPerSecond - older) % kCyclesPerSecond;
}

bool rollingTxRequested() {
    const char* value = std::getenv("MACFW_44_ROLLING_TX");
    return value && value[0] != '\0' && value[0] != '0';
}

std::size_t rollingTxLeadPackets() {
    const char* value = std::getenv("MACFW_44_ROLLING_TX_CYCLES");
    if (!value || value[0] == '\0')
        return kRollingTxDefaultLeadPackets;
    char* end = nullptr;
    const auto parsed = std::strtoull(value, &end, 10);
    if (!end || *end != '\0' || parsed < 16 || parsed >= kTxPackets)
        return 0;
    return static_cast<std::size_t>(parsed);
}

std::size_t rollingLivePcmReserveFrames() {
    const char* value = std::getenv("MACFW_44_ROLLING_PCM_RESERVE_FRAMES");
    if (!value || value[0] == '\0')
        return kRollingLivePcmReserveFrames;
    char* end = nullptr;
    const auto parsed = std::strtoull(value, &end, 10);
    if (!end || *end != '\0' || parsed < 64 ||
        parsed > kLivePcmReserveFrames)
        return 0;
    return static_cast<std::size_t>(parsed);
}

bool preloadDiagnosticAudio(macfw::PcmRingBuffer& pcm,
                            double frequency,
                            std::size_t frames) {
    if (!pcm.valid() || !std::isfinite(frequency) || frequency < 0.0 ||
        frequency >= static_cast<double>(kRate) / 2.0 || frames == 0 ||
        frames > pcm.capacityFrames())
        return false;

    constexpr double amplitude = 0.06309573444801933; // -24 dBFS peak
    std::vector<std::int32_t> samples(
        frames * macfw::fw1814::kPlaybackPcmPositions, 0);
    const std::size_t position =
        macfw::fw1814::kPlaybackPositionForAnalogOutput[0];
    if (frequency > 0.0) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const double phase = 2.0 * kPi * frequency *
                static_cast<double>(frame) / static_cast<double>(kRate);
            samples[frame * macfw::fw1814::kPlaybackPcmPositions + position] =
                static_cast<std::int32_t>(
                    std::sin(phase) * amplitude * 8388607.0);
        }
    }
    const std::size_t written = pcm.write(samples.data(), frames);
    std::cout << "FW1814 44.1 diagnostic PCM preload: " << written << '/'
              << frames << " frames, ";
    if (frequency == 0.0)
        std::cout << "digital silence\n";
    else
        std::cout << frequency << " Hz on Analog Output 1\n";
    return written == frames;
}

bool serviceTxFor(IOFireWireLibDeviceRef native,
                  macfw::fw1814::transport::BlockingPcmStream44100& streamer,
                  double seconds) {
    if (!native || seconds < 0.0) return false;
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;
    while (!gStopRequested && CFAbsoluteTimeGetCurrent() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.00025, false);
        UInt32 cycleTime = 0;
        if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess)
            return false;
        streamer.service(cycleCount(cycleTime));
        if (!streamer.healthy()) {
            std::cerr << "FW1814 44.1 rolling TX deadline missed during startup\n";
            return false;
        }
    }
    return !gStopRequested;
}

bool run() {
    using namespace macfw::fw1814::transport;

    SharedPlaybackReader playbackShared;
    if (!playbackShared.open()) {
        std::cerr << "FW1814 playback shared ring unavailable; install/restart the HAL first\n";
        return false;
    }
    if (playbackShared.ring()->sampleRate.load(std::memory_order_acquire) != kRate) {
        std::cerr << "FW1814 playback shared ring is not set to 44100 Hz\n";
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
        auto native = device.nativeHandle();
        UInt32 cycleTime = 0;
        if (!native || (*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) {
            std::cerr << "FW1814 GetCycleTime failed\n";
            goto cleanup;
        }
        const UInt32 initialCycle = cycleCount(cycleTime);
        const bool rollingTx = rollingTxRequested();
        const UInt32 cycleLead =
            rollingTx ? kRollingCycleLead : kValidatedCycleLead;
        const UInt32 firstCycle =
            (initialCycle + cycleLead) % kCyclesPerSecond;

        macfw::PcmRingBuffer pcm(kPcmCapacityFrames,
                                 macfw::fw1814::kPlaybackPcmPositions);
        if (const char* diagnosticTone =
                std::getenv("MACFW_44_PRELOAD_TONE_HZ")) {
            const double frequency = std::strtod(diagnosticTone, nullptr);
            if (!preloadDiagnosticAudio(pcm, frequency, 5 * kRate)) {
                std::cerr << "FW1814 44.1 diagnostic audio preload failed\n";
                goto cleanup;
            }
        } else if (std::getenv("MACFW_44_PRIME_SILENCE")) {
            if (!preloadDiagnosticAudio(pcm, 0.0, kPrimePcmFrames)) {
                std::cerr << "FW1814 44.1 prime-silence diagnostic failed\n";
                goto cleanup;
            }
        } else if (const char* diagnosticFrames =
                       std::getenv("MACFW_44_PRIME_SILENCE_FRAMES")) {
            const std::size_t frames = static_cast<std::size_t>(
                std::strtoull(diagnosticFrames, nullptr, 10));
            if (!preloadDiagnosticAudio(pcm, 0.0, frames)) {
                std::cerr << "FW1814 44.1 diagnostic silence preload failed\n";
                goto cleanup;
            }
        } else if (!rollingTx) {
            if (!preloadDiagnosticAudio(pcm, 0.0, kWarmupPcmFrames)) {
                std::cerr << "FW1814 44.1 silence preload failed\n";
                goto cleanup;
            }
        } else {
            std::cout << "FW1814 44.1 rolling startup: legacy 2-second PCM "
                         "preload bypassed\n";
        }
        auto rx = macfw::AmdtpReceiveRing::create(
            device, kCaptureSlots, kCaptureMaxPacket);
        auto tx = BlockingPcmTransmitRing44100::create(
            device, firstCycle, kTxPackets);
        if (!pcm.valid() || !rx || !tx) {
            std::cerr << "FW1814 44.1 PCM/ISO ring creation failed\n";
            goto cleanup;
        }

        BlockingPcmStream44100 streamer(
            tx, pcm, initialCycle, firstCycle, kTxHalfPackets);
        const std::size_t rollingLead = rollingTx ? rollingTxLeadPackets() : 0;
        const std::size_t rollingGuard = rollingLead / 2;
        const std::size_t livePcmReserve = rollingTx
            ? rollingLivePcmReserveFrames()
            : kLivePcmReserveFrames;
        if (rollingTx && rollingLead == 0) {
            std::cerr << "FW1814 invalid 44.1 rolling TX configuration; "
                         "MACFW_44_ROLLING_TX_CYCLES must be 16..639\n";
            goto cleanup;
        }
        if (rollingTx && livePcmReserve == 0) {
            std::cerr << "FW1814 invalid 44.1 rolling PCM reserve; "
                         "MACFW_44_ROLLING_PCM_RESERVE_FRAMES must be 64..2048\n";
            goto cleanup;
        }
        if (!streamer.valid() || !streamer.prime()) {
            std::cerr << "FW1814 44.1 playback stream prime failed\n";
            goto cleanup;
        }

        std::cout << "FW1814 44.1 playback TX ring: " << kTxPackets
                  << " packets / " << kTxHalfPackets
                  << "-packet halves (80 ms / 40 ms)\n";
        if (rollingTx) {
            std::cout << "FW1814 44.1 EXPERIMENTAL rolling TX: "
                      << rollingLead << "-cycle lead ("
                      << rollingLead * 1000 / kCyclesPerSecond
                      << " ms), " << rollingGuard
                      << "-cycle deadline guard, " << livePcmReserve
                      << "-frame live PCM reserve\n";
        } else {
            std::cout << "FW1814 44.1 rolling TX: disabled; "
                         "validated half-ring refill active\n";
        }
        std::cout << "FW1814 44.1 capture RX ring: " << kCaptureSlots
                  << " packets / 32-packet publication chunks\n";
        std::cout << "FW1814 44.1 scheduled TX lead: " << cycleLead
                  << " cycles (" << cycleLead * 1000 / kCyclesPerSecond
                  << " ms)\n";

        if (!lifecycle.prepare(device, rx, tx.nativeLocalPort(),
                               kCaptureMaxPacket, kPlaybackMaxPacket)) {
            std::cerr << "FW1814 44.1 duplex lifecycle prepare failed\n";
            goto cleanup;
        }
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
                      << " Hz; run make -C devices/fw1814/tools init-44 first\n";
            goto cleanup;
        }

        if (!lifecycle.startIsoch(isochCallbackThread.runLoop())) {
            std::cerr << "FW1814 44.1 duplex ISO/CMP start failed\n";
            goto cleanup;
        }
        isochCallbackThread.startPumping();

        std::cout << "FW1814 44.1 duplex ISO started: playback ch="
                  << lifecycle.playbackChannel()
                  << " capture ch=" << lifecycle.captureChannel() << '\n';
        std::cout << "FW1814 isoch callback dispatcher: dedicated run-loop thread\n";

        UInt32 nowCycleTime = 0;
        if ((*native)->GetCycleTime(native, &nowCycleTime) != kIOReturnSuccess)
            goto cleanup;
        const UInt32 nowCycle = cycleCount(nowCycleTime);
        const UInt32 forward = cycleDelta(firstCycle, nowCycle);
        if (forward > 4096u) {
            std::cerr << "FW1814 44.1 scheduled first cycle is no longer safely ahead\n";
            goto cleanup;
        }
        const double startupWait =
            static_cast<double>(forward) / kCyclesPerSecond + 0.020;
        std::cout << "FW1814 servicing native 44.1 TX for " << std::fixed
                  << std::setprecision(3) << startupWait
                  << " s through scheduled first-cycle start\n"
                  << std::defaultfloat;
        if (!serviceTxFor(native, streamer, startupWait))
            goto cleanup;

        std::cout << "FW1814 special 44.1 stream kick: OUTPUT 44100 Hz\n";
        if (!fcp.setSignalRate(kRate, 0x18, false)) {
            std::cerr << "FW1814 OUTPUT 44100 CONTROL failed\n";
            goto cleanup;
        }

        std::cout << "FW1814 special 44.1 stream kick: servicing TX for 100 ms before INPUT\n";
        if (!serviceTxFor(native, streamer, 0.100))
            goto cleanup;

        std::cout << "FW1814 special 44.1 stream kick: INPUT 44100 Hz\n";
        if (!fcp.setSignalRate(kRate, 0x19, false)) {
            std::cerr << "FW1814 INPUT 44100 CONTROL failed\n";
            goto cleanup;
        }

        if (!serviceTxFor(native, streamer, 0.050))
            goto cleanup;

        unsigned readback = 0;
        if (!fcp.readInputRate(readback, false) || readback != kRate) {
            std::cerr << "FW1814 post-start INPUT rate readback failed: "
                      << readback << " Hz\n";
            goto cleanup;
        }
        std::cout << "FW1814 post-start INPUT rate readback: 44100 Hz PASS\n";

        if (!control.start(device, kRate))
            std::cerr << "warning: FW1814 control socket unavailable; audio will continue\n";

        // Linux can keep its kernel DMA queue moving while the M-Audio rate
        // command completes. This user-space transport cannot safely service
        // the rolling horizon during the synchronous FCP call, so retain the
        // primed half-ring only for that startup operation and switch
        // immediately afterward.
        if (rollingTx && !streamer.enableRolling(rollingLead, rollingGuard)) {
            std::cerr << "FW1814 could not enable 44.1 rolling TX\n";
            goto cleanup;
        }

        signalEngineReady();

        playbackShared.ring()->active.store(1, std::memory_order_release);
        playbackActive = true;

        CapturePump44100 capturePump;
        PlaybackPumpStats playbackPumpStats;
        std::vector<float> audio(
            4096 * macfw::fw1814::hal::kOutputChannels, 0.0f);
        std::vector<std::int32_t> mapped(
            4096 * macfw::fw1814::kPlaybackPcmPositions, 0);

        std::cout << "FW1814 44.1 analog engine ONLINE\n"
                  << "    CoreAudio-facing outputs: Analog 1-4\n"
                  << "    CoreAudio-facing inputs:  Analog 1-8\n"
                  << "    digital/MIDI/headphone levels: deferred\n"
                  << "    audio service: dedicated Mach-paced thread (250 us)\n"
                  << "    Ctrl-C to stop\n";

        std::atomic<bool> audioFinished{false};
        bool audioOk = false;
        const bool playbackOnlyDiagnostic =
            std::getenv("MACFW_44_PLAYBACK_ONLY") != nullptr;
        if (playbackOnlyDiagnostic)
            std::cout << "FW1814 44.1 diagnostic: capture pumping disabled\n";

        std::thread audioThread([&] {
            requestInteractiveQos("FW1814 44.1 audio service thread");
            requestAudioTimeConstraint();
            MachPacer pacer(kAudioServicePeriodNs);
            if (!pacer.valid()) {
                std::cerr << "FW1814 44.1 Mach pacing setup failed\n";
                audioFinished.store(true, std::memory_order_release);
                return;
            }

            bool captureReady = false;
            const bool verbose = std::getenv("MACFW_VERBOSE") != nullptr;
            CFAbsoluteTime lastGenerationCheck = CFAbsoluteTimeGetCurrent();
            CFAbsoluteTime lastStatus = lastGenerationCheck;
            std::uint64_t lastCaptureFrames = 0;
            bool awaitingLivePlayback = true;
            const CFAbsoluteTime playbackWaitStart = CFAbsoluteTimeGetCurrent();

            while (!gStopRequested) {
                pacer.wait();

                if (!playbackOnlyDiagnostic)
                    capturePump.service(rx, *captureShared.ring());
                if (awaitingLivePlayback) {
                    // CoreAudio can write during the preload. Drop those old
                    // frames so the first admitted sample is current audio.
                    playbackShared.discardBacklog();
                    if (pcm.availableFrames() <= livePcmReserve) {
                        awaitingLivePlayback = false;
                        std::cout << "FW1814 44.1 live playback enabled after "
                                  << CFAbsoluteTimeGetCurrent() - playbackWaitStart
                                  << " s; PCM reserve=" << pcm.availableFrames()
                                  << " frames\n";
                    }
                } else {
                    drainPlayback(*playbackShared.ring(), pcm, audio, mapped,
                                  &playbackPumpStats);
                }

                UInt32 serviceCycleTime = 0;
                if ((*native)->GetCycleTime(native, &serviceCycleTime) == kIOReturnSuccess)
                    streamer.service(cycleCount(serviceCycleTime));

                if (!streamer.healthy()) {
                    const auto& txStats = streamer.stats();
                    std::cerr << "FW1814 44.1 rolling TX deadline missed; "
                                 "stopping before unsafe slot reuse (misses="
                              << txStats.rollingDeadlineMisses
                              << ", max-cycle-gap=" << txStats.maxCycleDelta
                              << ")\n";
                    audioFinished.store(true, std::memory_order_release);
                    return;
                }

                if (!playbackOnlyDiagnostic)
                    capturePump.service(rx, *captureShared.ring());

                // The HAL may suspend a stale/full capture queue while the
                // client is not reading. Reflect that transition locally so
                // the transport can prefill and reactivate capture again.
                if (!playbackOnlyDiagnostic && captureReady &&
                    captureShared.ring()->active.load(std::memory_order_acquire) == 0)
                    captureReady = false;
                if (!playbackOnlyDiagnostic && !captureReady &&
                    captureShared.activateForConsumer(kCapturePrefillFrames)) {
                    captureReady = true;
                    std::cout << "FW1814 44.1 capture consumer detected; live capture enabled\n";
                }

                const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
                if (now - lastGenerationCheck >= 0.25) {
                    if (!lifecycle.generationStillValid()) {
                        std::cerr << "FW1814 FireWire generation changed during 44.1 streaming; requesting engine restart\n";
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
                    std::cout << "FW1814-44 out-shared="
                              << macfw::fw1814::hal::availableFrames(*pb)
                              << " pcm=" << pcm.availableFrames()
                              << " tx-audio=" << txStats.framesFromBuffer
                              << " tx-silence=" << txStats.framesSilenced
                              << " tx-late=" << txStats.lateCyclePolls
                              << " tx-max-gap=" << txStats.maxCycleDelta
                              << " tx-roll-packets="
                              << txStats.rollingPacketsRefilled
                              << " tx-roll-miss="
                              << txStats.rollingDeadlineMisses
                              << " marker-pb="
                              << playbackPumpStats.firstLoudHostTime
                              << " marker-tx=" << txStats.firstLoudHostTime
                              << " marker-cap=" << rxStats.firstLoudHostTime
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
                              << " repeat-ts=" << rxStats.repeatedTerminalTimestamps
                              << " malformed=" << captureShared.ring()->malformedPackets.load()
                              << " invalid=" << captureShared.ring()->invalidLabels.load()
                              << " nodata=" << rxStats.noDataPackets
                              << " dbc-gap=" << rxStats.dbcDiscontinuities
                              << " ts-regress=" << rxStats.timestampRegressions
                              << " reorder=" << rxStats.reorderedPackets
                              << " stale=" << rxStats.stalePackets << '\n';
                    lastCaptureFrames = captureFrames;
                    lastStatus = now;
                }
            }

            audioOk = true;
            audioFinished.store(true, std::memory_order_release);
        });

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
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    std::cout << "macfw fw1814analog44 — experimental 44.1 kHz analog full-duplex engine\n";
    return run() ? 0 : 1;
}
