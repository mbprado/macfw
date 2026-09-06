#include "../fcp_control.h"
#include "../special_mixer.h"
#include "blocking_pcm_tx.h"
#include "capture_pump.h"
#include "duplex_lifecycle.h"
#include "pcm_stream48.h"
#include "playback_pump.h"
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
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kTxPackets = 128;
constexpr std::size_t kTxHalfPackets = 64;
constexpr std::size_t kPcmCapacityFrames = 16384;
constexpr std::size_t kCapturePrefillFrames = 512;
constexpr UInt32 kCycleLead = 256;
constexpr UInt32 kCyclesPerSecond = 8000;

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

        if (!lifecycle.prepare(device, rx, tx.nativeLocalPort(),
                               kCaptureMaxPacket, kPlaybackMaxPacket)) {
            std::cerr << "FW1814 duplex lifecycle prepare failed\n";
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

        unsigned currentRate = 0;
        if (!fcp.readInputRate(currentRate, false) || currentRate != kRate) {
            std::cerr << "FW1814 authoritative INPUT rate is " << currentRate
                      << " Hz; run make -C devices/fw1814/tools init-48 first\n";
            goto cleanup;
        }

        if (!lifecycle.startIsoch()) {
            std::cerr << "FW1814 duplex ISO/CMP start failed\n";
            goto cleanup;
        }

        std::cout << "FW1814 duplex ISO started: playback ch="
                  << lifecycle.playbackChannel()
                  << " capture ch=" << lifecycle.captureChannel() << '\n';
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

        playbackShared.ring()->active.store(1, std::memory_order_release);
        playbackActive = true;

        CapturePump48k capturePump;
        std::vector<float> audio(
            4096 * macfw::fw1814::hal::kOutputChannels, 0.0f);
        std::vector<std::int32_t> mapped(
            4096 * macfw::fw1814::kPlaybackPcmPositions, 0);

        bool captureReady = false;
        const bool verbose = std::getenv("MACFW_VERBOSE") != nullptr;
        CFAbsoluteTime lastGenerationCheck = CFAbsoluteTimeGetCurrent();
        CFAbsoluteTime lastStatus = lastGenerationCheck;
        std::uint64_t lastCaptureFrames = 0;

        std::cout << "FW1814 analog engine ONLINE\n"
                  << "    CoreAudio-facing outputs: Analog 1-4\n"
                  << "    CoreAudio-facing inputs:  Analog 1-8\n"
                  << "    digital/MIDI/headphone routing: deferred\n"
                  << "    Ctrl-C to stop\n";

        while (!gStopRequested) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.00025, false);

            capturePump.service(rx, *captureShared.ring());
            drainPlayback(*playbackShared.ring(), pcm, audio, mapped);

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
                    goto cleanup;
                }
                lastGenerationCheck = now;
            }

            if (verbose && now - lastStatus >= 2.0) {
                const auto captureFrames =
                    captureShared.ring()->decodedFrames.load(std::memory_order_acquire);
                const auto& txStats = streamer.stats();
                const auto& rxStats = capturePump.stats();
                std::cout << "FW1814 out-shared="
                          << macfw::fw1814::hal::availableFrames(*playbackShared.ring())
                          << " pcm=" << pcm.availableFrames()
                          << " tx-silence=" << txStats.framesSilenced
                          << " tx-late=" << txStats.lateCyclePolls
                          << " | capture=" << captureFrames
                          << " (delta " << (captureFrames - lastCaptureFrames) << ')'
                          << " queued="
                          << macfw::fw1814::hal::capture::availableFrames(*captureShared.ring())
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

        ok = true;
    }

cleanup:
    if (playbackActive)
        playbackShared.ring()->active.store(0, std::memory_order_release);

    // No FCP/AVC is issued after a failed stream kick. Local ISO always stops;
    // PCR restore is attempted only while the original generation is valid.
    const bool restoreOk = lifecycle.stopIsochAndRestoreCmp();
    fcp.reset();
    lifecycle.removeDispatchers();
    if (!restoreOk && ok) ok = false;
    device.close();
    return ok;
}

} // namespace

int main() {
    gStopRequested = 0;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::cout << "macfw fw1814analog48 — experimental 48 kHz analog full-duplex engine\n";
    return run() ? 0 : 1;
}
