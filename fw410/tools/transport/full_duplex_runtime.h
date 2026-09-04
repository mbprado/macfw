#pragma once

#include "capture_shared.h"
#include "full_duplex_shared.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/pcm_ring_buffer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <mach/mach_time.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace macfw::transport::duplex {

struct FullDuplexRuntimeConfig {
    const char* rateLabel = "";
    unsigned prefillMilliseconds = 0;
    double runLoopSliceSeconds = 0.001;
    bool printIsoStarted = false;
    bool printTransportGeometry = false;
    UInt32 expectedGeneration = 0;
    double generationCheckIntervalSeconds = 0.25;
};

template <typename Streamer, typename PlaybackService>
bool runFullDuplexServiceLoop(
    volatile std::sig_atomic_t& stopRequested,
    IOFireWireLibDeviceRef native,
    SharedPlaybackReader& input,
    macfw::PcmRingBuffer& pcm,
    macfw::AmdtpReceiveRing& rx,
    macfw::transport::CaptureSharedWriter& captureShared,
    macfw::transport::CaptureReceivePump& capturePump,
    Streamer& streamer,
    const FullDuplexRuntimeConfig& config,
    PlaybackService&& playbackService) {

    std::vector<float> audio(4096 * macfw::hal::kChannels, 0.0f);
    std::vector<std::int32_t> mapped(4096 * kPcmChannels, 0);
    std::uint64_t lastWrite = input.ring()->writeFrame.load(std::memory_order_acquire);
    std::uint64_t lastCaptureFrames = 0;
    CFAbsoluteTime lastStatus = CFAbsoluteTimeGetCurrent();
    CFAbsoluteTime lastGenerationCheck = lastStatus;
    bool captureReady = false;
    const bool verboseDiagnostics = std::getenv("MACFW_VERBOSE") != nullptr;

    mach_timebase_info_data_t timebase{};
    if (verboseDiagnostics) mach_timebase_info(&timebase);
    const auto ticksToMicroseconds = [&](std::uint64_t ticks) -> std::uint64_t {
        if (!verboseDiagnostics || timebase.denom == 0) return 0;
        return (ticks * static_cast<std::uint64_t>(timebase.numer)) /
               static_cast<std::uint64_t>(timebase.denom) / 1000u;
    };

    std::uint64_t previousLoopStart = 0;
    std::uint64_t maxLoopPeriodUs = 0;
    std::uint64_t maxRunLoopUs = 0;
    std::uint64_t maxServiceUs = 0;
    std::uint64_t loopOver1ms = 0;
    std::uint64_t loopOver2ms = 0;
    std::uint64_t loopOver4ms = 0;
    std::uint64_t loopOver8ms = 0;

    if (config.printIsoStarted) std::cout << "duplex ISO started\n";
    if (config.printTransportGeometry)
        std::cout << "TX ring: 640 cycles / 320-cycle refill halves\nPCM FIFO: 16384 frames\n";

    std::cout << "CoreAudio outputs: Analog 1-8, S/PDIF L/R (10 channels)\n"
              << "CoreAudio inputs: Analog In 1-2, S/PDIF In L/R (4 channels)\n"
              << "capture prefill: " << kCapturePrefillFrames << " frames (~"
              << config.prefillMilliseconds << " ms)\n"
              << "capture receive: terminal-slot completed 32-cycle chunks\n"
              << "HAL bridge active: full-duplex " << config.rateLabel
              << " kHz playback + capture; Ctrl-C to stop\n";

    while (!stopRequested) {
        const std::uint64_t loopStart = verboseDiagnostics ? mach_absolute_time() : 0;
        if (verboseDiagnostics && previousLoopStart != 0) {
            const std::uint64_t periodUs = ticksToMicroseconds(loopStart - previousLoopStart);
            if (periodUs > maxLoopPeriodUs) maxLoopPeriodUs = periodUs;
            if (periodUs > 1000u) ++loopOver1ms;
            if (periodUs > 2000u) ++loopOver2ms;
            if (periodUs > 4000u) ++loopOver4ms;
            if (periodUs > 8000u) ++loopOver8ms;
        }
        previousLoopStart = loopStart;

        CFRunLoopRunInMode(kCFRunLoopDefaultMode, config.runLoopSliceSeconds, false);

        const std::uint64_t afterRunLoop = verboseDiagnostics ? mach_absolute_time() : 0;
        if (verboseDiagnostics) {
            const std::uint64_t runLoopUs = ticksToMicroseconds(afterRunLoop - loopStart);
            if (runLoopUs > maxRunLoopUs) maxRunLoopUs = runLoopUs;
        }

        const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        if (config.expectedGeneration != 0 &&
            now - lastGenerationCheck >= config.generationCheckIntervalSeconds) {
            UInt32 currentGeneration = 0;
            const IOReturn generationResult =
                (*native)->GetBusGeneration(native, &currentGeneration);
            if (generationResult != kIOReturnSuccess) {
                std::cerr << "FireWire generation read failed during streaming: 0x"
                          << std::hex << generationResult << std::dec
                          << "; requesting transport restart\n";
                return false;
            }
            if (currentGeneration != config.expectedGeneration) {
                std::cerr << "FireWire generation changed "
                          << config.expectedGeneration << " -> " << currentGeneration
                          << "; requesting transport restart\n";
                return false;
            }
            lastGenerationCheck = now;
        }

        capturePump.service(rx, *captureShared.ring());
        playbackService(audio, mapped);

        UInt32 nowCt = 0;
        if ((*native)->GetCycleTime(native, &nowCt) == kIOReturnSuccess)
            streamer.service(cycleCount(nowCt));

        capturePump.service(rx, *captureShared.ring());

        if (!captureReady && captureShared.activateForConsumer(kCapturePrefillFrames)) {
            captureReady = true;
            std::cout << "capture prefill ready: CoreAudio consumer detected; live capture enabled\n";
        }

        if (verboseDiagnostics) {
            const std::uint64_t serviceEnd = mach_absolute_time();
            const std::uint64_t serviceUs = ticksToMicroseconds(serviceEnd - afterRunLoop);
            if (serviceUs > maxServiceUs) maxServiceUs = serviceUs;
        }

        if (!verboseDiagnostics) continue;
        if (now - lastStatus < 2.0) continue;

        const auto w = input.ring()->writeFrame.load(std::memory_order_acquire);
        const auto captureFrames = captureShared.ring()->decodedFrames.load(std::memory_order_acquire);
        const auto& txStats = streamer.stats();
        const auto& rxStats = capturePump.stats();

        std::cout << "HAL out=" << w << " (delta " << (w - lastWrite) << ")"
                  << " shared=" << macfw::hal::availableFrames(*input.ring())
                  << " pcm=" << pcm.availableFrames()
                  << " out-drops=" << input.ring()->droppedFrames.load()
                  << " tx-late=" << txStats.lateCyclePolls
                  << " tx-silence=" << txStats.framesSilenced
                  << " | capture=" << captureFrames << " (delta " << (captureFrames - lastCaptureFrames) << ")"
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
                  << " | timing-us(loop/runloop/service)="
                  << maxLoopPeriodUs << '/' << maxRunLoopUs << '/' << maxServiceUs
                  << " loop>1/2/4/8ms="
                  << loopOver1ms << '/' << loopOver2ms << '/' << loopOver4ms << '/' << loopOver8ms
                  << '\n';

        lastWrite = w;
        lastCaptureFrames = captureFrames;
        lastStatus = now;
        maxLoopPeriodUs = 0;
        maxRunLoopUs = 0;
        maxServiceUs = 0;
        loopOver1ms = 0;
        loopOver2ms = 0;
        loopOver4ms = 0;
        loopOver8ms = 0;
    }
    return true;
}

} // namespace macfw::transport::duplex
