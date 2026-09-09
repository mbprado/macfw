#include "../channel_map.h"
#include "../fcp_control.h"
#include "../special_mixer.h"
#include "../transport/blocking_pcm_tx44.h"
#include "../transport/duplex_lifecycle.h"
#include "../transport/realtime_service.h"

#include "macfw/amdtp_receive_ring.h"
#include "macfw/firewire_device.h"
#include "macfw/pcm_ring_buffer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace macfw::fw1814::transport;

constexpr const char* kProduct = "FW 1814";
constexpr unsigned kRate = 44100;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr UInt32 kCycleLead = 2048;
constexpr UInt32 kCaptureMaxPacket = 360;
constexpr UInt32 kPlaybackMaxPacket = 232;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kTxPackets = 640;
constexpr std::size_t kTxHalfPackets = 320;
constexpr std::size_t kPcmCapacityFrames = 262144;
constexpr double kToneRunSeconds = 3.0;
constexpr double kPreloadSeconds = 5.0;
constexpr double kPi = 3.14159265358979323846;

UInt32 cycleCount(UInt32 cycleTime) {
    return (cycleTime >> 12) & 0x1fffu;
}

UInt32 cycleDelta(UInt32 newer, UInt32 older) {
    return (newer + kCyclesPerSecond - older) % kCyclesPerSecond;
}

bool serviceFor(IOFireWireLibDeviceRef native,
                BlockingPcmStream44100& streamer,
                double seconds) {
    if (!native || seconds < 0.0) return false;
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;
    while (CFAbsoluteTimeGetCurrent() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.00025, false);
        UInt32 cycleTime = 0;
        if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess)
            return false;
        streamer.service(cycleCount(cycleTime));
    }
    return true;
}

bool preloadTone(macfw::PcmRingBuffer& pcm,
                 std::size_t physicalOutput,
                 double frequencyHz) {
    if (!pcm.valid() || physicalOutput < 1 ||
        physicalOutput > macfw::fw1814::kAnalogOutputCount ||
        frequencyHz <= 0.0)
        return false;

    const std::size_t rawPosition =
        macfw::fw1814::playbackPositionForAnalogOutput(physicalOutput);
    if (rawPosition >= pcm.channelCount()) return false;

    const std::size_t frames = static_cast<std::size_t>(kPreloadSeconds * kRate);
    if (frames > pcm.capacityFrames()) return false;

    std::vector<std::int32_t> samples(frames * pcm.channelCount(), 0);
    const double amplitude = 8388607.0 * std::pow(10.0, -24.0 / 20.0);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double phase = 2.0 * kPi * frequencyHz *
                             static_cast<double>(frame) /
                             static_cast<double>(kRate);
        samples[frame * pcm.channelCount() + rawPosition] =
            static_cast<std::int32_t>(std::sin(phase) * amplitude);
    }

    const std::size_t written = pcm.write(samples.data(), frames);
    std::cout << "FW1814 44.1 tone preload:\n"
              << "    physical output: Analog " << physicalOutput << '\n'
              << "    raw PCM position: " << rawPosition << '\n'
              << "    frequency: " << frequencyHz << " Hz\n"
              << "    level: -24 dBFS\n"
              << "    preloaded frames: " << written << '/' << frames << '\n';
    return written == frames;
}

bool run(std::size_t physicalOutput, double frequencyHz) {
    auto device = macfw::FireWireDevice::findByProductName(kProduct);
    if (!device) {
        std::cerr << "No operational FW 1814 unit found\n";
        return false;
    }
    if (device.open() != kIOReturnSuccess) {
        std::cerr << "FW1814 open failed\n";
        return false;
    }

    std::cout << "matched operational FW1814:\n"
              << "    generation: " << device.generation() << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID()
              << std::dec << '\n';

    auto native = device.nativeHandle();
    UInt32 initialCycleTime = 0;
    if (!native || (*native)->GetCycleTime(native, &initialCycleTime) !=
                       kIOReturnSuccess) {
        std::cerr << "FW1814 GetCycleTime failed\n";
        device.close();
        return false;
    }

    if (!macfw::fw1814::applyStraightAnalogPlaybackRouting(device)) {
        std::cerr << "FW1814 known analog routing write failed\n";
        device.close();
        return false;
    }

    const UInt32 initialCycle = cycleCount(initialCycleTime);
    const UInt32 firstCycle = (initialCycle + kCycleLead) % kCyclesPerSecond;

    macfw::PcmRingBuffer pcm(kPcmCapacityFrames,
                             BlockingPcmTransmitRing44100::pcmChannels());
    if (!preloadTone(pcm, physicalOutput, frequencyHz)) {
        std::cerr << "FW1814 44.1 tone preload failed\n";
        device.close();
        return false;
    }

    auto rx = macfw::AmdtpReceiveRing::create(
        device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = BlockingPcmTransmitRing44100::create(
        device, firstCycle, kTxPackets);
    BlockingPcmStream44100 streamer(
        tx, pcm, initialCycle, firstCycle, kTxHalfPackets);

    if (!rx || !tx || !streamer.valid() || !streamer.prime()) {
        std::cerr << "FW1814 44.1 tone transport ring setup/prime failed\n";
        device.close();
        return false;
    }

    DuplexLifecycle lifecycle;
    macfw::fw1814::FcpControl fcp;
    IsochCallbackRunLoopThread isochThread;
    bool ok = false;
    bool fcpArmed = false;

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
    fcpArmed = true;
    if (!isochThread.prepare()) {
        std::cerr << "FW1814 isoch callback thread setup failed\n";
        goto cleanup;
    }

    {
        unsigned currentRate = 0;
        const bool rateOk = fcp.readInputRate(currentRate) &&
                            currentRate == kRate;
        std::cout << "    authoritative INPUT rate: " << currentRate
                  << " Hz -> " << (rateOk ? "PASS" : "FAIL") << '\n';
        if (!rateOk) {
            std::cerr << "Run make -C devices/fw1814/tools init-44 first\n";
            goto cleanup;
        }
    }

    if (!lifecycle.startIsoch(isochThread.runLoop())) {
        std::cerr << "FW1814 44.1 duplex ISO/CMP start failed\n";
        goto cleanup;
    }
    isochThread.startPumping();

    std::cout << "FW1814 native 44.1 tone ISO started: playback ch="
              << lifecycle.playbackChannel()
              << " capture ch=" << lifecycle.captureChannel() << '\n';

    {
        UInt32 nowCycleTime = 0;
        if ((*native)->GetCycleTime(native, &nowCycleTime) != kIOReturnSuccess)
            goto cleanup;
        const UInt32 now = cycleCount(nowCycleTime);
        const UInt32 forward = cycleDelta(firstCycle, now);
        if (forward > 4096u) {
            std::cerr << "FW1814 44.1 scheduled first cycle is no longer safely ahead\n";
            goto cleanup;
        }
        const double waitSeconds =
            static_cast<double>(forward) / kCyclesPerSecond + 0.020;
        std::cout << "FW1814 servicing preloaded tone for " << std::fixed
                  << std::setprecision(3) << waitSeconds
                  << " s through scheduled first-cycle start\n"
                  << std::defaultfloat;
        if (!serviceFor(native, streamer, waitSeconds)) goto cleanup;
    }

    std::cout << "FW1814 special 44.1 stream kick: OUTPUT 44100 Hz\n";
    if (!fcp.setSignalRate(kRate, 0x18)) goto cleanup;

    std::cout << "FW1814 special 44.1 stream kick: servicing tone for 100 ms before INPUT\n";
    if (!serviceFor(native, streamer, 0.100)) goto cleanup;

    std::cout << "FW1814 special 44.1 stream kick: INPUT 44100 Hz\n";
    if (!fcp.setSignalRate(kRate, 0x19)) goto cleanup;

    if (!serviceFor(native, streamer, 0.050)) goto cleanup;

    {
        unsigned readback = 0;
        if (!fcp.readInputRate(readback) || readback != kRate) {
            std::cerr << "FW1814 post-start INPUT rate readback failed: "
                      << readback << " Hz\n";
            goto cleanup;
        }
        std::cout << "FW1814 post-start INPUT rate readback: "
                  << readback << " Hz PASS\n";
    }

    std::cout << "FW1814 playing native 44.1 tone on Analog Output "
              << physicalOutput << " for " << kToneRunSeconds << " seconds\n";
    if (!serviceFor(native, streamer, kToneRunSeconds)) goto cleanup;

    {
        const auto& stats = streamer.stats();
        std::cout << "FW1814 44.1 tone result:\n"
                  << "    TX halves refilled: " << stats.halvesRefilled << '\n'
                  << "    TX data packets refilled: "
                  << stats.dataPacketsRefilled << '\n'
                  << "    TX tone frames: " << stats.framesFromBuffer << '\n'
                  << "    TX silence frames: " << stats.framesSilenced << '\n'
                  << "    TX late cycle polls: " << stats.lateCyclePolls
                  << " (FCP waits included)\n"
                  << "    PCM remaining frames: " << pcm.availableFrames() << '\n';

        ok = stats.framesFromBuffer != 0 &&
             stats.framesSilenced == 0 &&
             pcm.availableFrames() != 0 &&
             lifecycle.generationStillValid();
        std::cout << "status: " << (ok ? "PASS" : "FAIL") << '\n';
    }

cleanup:
    if (!lifecycle.stopIsochAndRestoreCmp() && ok)
        ok = false;
    if (fcpArmed) fcp.reset();
    lifecycle.removeDispatchers();
    isochThread.stop();
    lifecycle.stop();
    device.close();
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: " << argv[0]
                  << " <analog-output-1..4> [frequency-hz]\n";
        return 64;
    }

    const long output = std::strtol(argv[1], nullptr, 10);
    const double frequency = argc == 3 ? std::strtod(argv[2], nullptr) : 440.0;
    if (output < 1 || output > 4 || frequency <= 0.0) {
        std::cerr << "invalid output or frequency\n";
        return 64;
    }

    std::cout << "macfw fw1814tone44 — native 44.1 kHz analog playback tone probe\n";
    return run(static_cast<std::size_t>(output), frequency) ? 0 : 1;
}
