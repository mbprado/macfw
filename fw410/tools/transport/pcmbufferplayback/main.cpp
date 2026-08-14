#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"
#include "macfw/pcm_buffer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr UInt32 kCaptureMaxPacket48k = 168;
constexpr UInt32 kPlaybackMaxPacket48k = 360;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kPlaybackSlots = 128;
constexpr std::size_t kPcmFrames = 768;
constexpr std::size_t kPcmChannels = 10;
constexpr UInt32 kCycleLead = 256;
constexpr double kPi = 3.14159265358979323846;

std::vector<std::int32_t> makeStereoTestBuffer() {
    std::vector<std::int32_t> pcm(kPcmFrames * kPcmChannels, 0);
    constexpr double amplitude = 131072.0;
    for (std::size_t frame = 0; frame < kPcmFrames; ++frame) {
        const double t = static_cast<double>(frame) / 48000.0;
        // FW410 physical Analog Output 1 = stream PCM position 2 (index 1).
        pcm[frame * kPcmChannels + 1] = static_cast<std::int32_t>(
            std::sin(2.0 * kPi * 440.0 * t) * amplitude);
        // FW410 physical Analog Output 2 = stream PCM position 7 (index 6).
        pcm[frame * kPcmChannels + 6] = static_cast<std::int32_t>(
            std::sin(2.0 * kPi * 880.0 * t) * amplitude);
    }
    return pcm;
}

void dumpCapture(const macfw::AmdtpReceiveRing& ring) {
    std::size_t touched = 0;
    std::size_t dataBearing = 0;
    for (std::size_t i = 0; i < ring.packetCount(); ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched()) continue;
        ++touched;
        if (slot.packet().length > 8) ++dataBearing;
    }
    std::cout << "capture summary:\n"
              << "    touched slots:      " << touched << " / " << ring.packetCount() << '\n'
              << "    data-bearing slots: " << dataBearing << '\n'
              << "result: " << (dataBearing ? "PASS" : "FAIL")
              << (dataBearing ? " - sample-bearing capture observed" : " - no sample-bearing capture")
              << '\n';
}

bool run(bool execute) {
    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) {
        std::cout << "No operational FW 410 unit found.\n";
        return false;
    }

    std::cout << "FW410 operational unit:\n"
              << "    generation: " << device.generation() << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID() << std::dec << '\n';

    if (device.open() != kIOReturnSuccess) {
        std::cout << "open failed\n";
        return false;
    }

    std::uint32_t opcr0 = 0, ipcr0 = 0;
    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) {
        std::cout << "PCR read failed\n";
        return false;
    }
    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n";
        return false;
    }

    auto pcmStorage = makeStereoTestBuffer();
    macfw::PcmBufferView pcm{pcmStorage.data(), kPcmFrames, kPcmChannels, true};

    std::cout << "PCM buffer test:\n"
              << "    sample rate:       48000 Hz\n"
              << "    frames:            " << pcm.frameCount << '\n'
              << "    stream positions:  " << pcm.channelCount << '\n'
              << "    loop:              yes\n"
              << "    Analog Output 1:   440 Hz (PCM position 2)\n"
              << "    Analog Output 2:   880 Hz (PCM position 7)\n"
              << "    amplitude:         ~-36 dBFS\n";

    auto native = device.nativeHandle();
    UInt32 cycleTime = 0;
    if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) return false;
    const UInt32 now = (cycleTime >> 12) & 0x1fffu;
    const UInt32 firstCycle = (now + kCycleLead) & 0x1fffu;

    if (!execute) {
        std::cout << "status: PASS - dry run only; no stream started\n"
                  << "to execute: ./pcmbufferplayback --execute\n";
        return true;
    }

    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket48k);
    auto tx = macfw::AmdtpTransmitRing::createPcm48k(
        device, firstCycle, pcm, kPlaybackSlots);
    auto capture = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::DeviceToHost, kCaptureMaxPacket48k);
    auto playback = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::HostToDevice, kPlaybackMaxPacket48k);

    if (!rx || !tx || !capture || !playback) {
        std::cout << "transport object creation failed\n";
        return false;
    }

    (*capture.nativeChannel())->AddListener(
        capture.nativeChannel(),
        reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
    if (playback.bindHostToDeviceTalkerFirst(tx.nativeLocalPort()) != kIOReturnSuccess) {
        std::cout << "playback port binding failed\n";
        return false;
    }

    bool callbackDispatcher = false;
    bool isochDispatcher = false;
    bool notifications = false;
    bool captureStarted = false;
    bool playbackStarted = false;
    bool opConnected = false;
    bool ipConnected = false;
    bool ok = false;

    if ((*native)->AddCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess)
        callbackDispatcher = true;
    if ((*native)->AddIsochCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess)
        isochDispatcher = true;
    if ((*native)->TurnOnNotification(native)) notifications = true;

    if (capture.allocate() != kIOReturnSuccess || playback.allocate() != kIOReturnSuccess) {
        std::cout << "ISO resource allocation failed\n";
        goto cleanup;
    }

    std::cout << "ISO resources:\n"
              << "    capture channel:  " << capture.channel() << '\n'
              << "    playback channel: " << playback.channel() << '\n';

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

    std::cout << "duplex ISO: started (direct interleaved PCM buffer)\n"
              << "observation window: 2.0 s\n"
              << "listen for 440 Hz on Analog Out 1 and 880 Hz on Analog Out 2\n";
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false);
    dumpCapture(rx);
    ok = true;

cleanup:
    if (playbackStarted) (*playback.nativeChannel())->Stop(playback.nativeChannel());
    if (captureStarted) (*capture.nativeChannel())->Stop(capture.nativeChannel());

    if (ipConnected) {
        const auto kr = macfw::cmp::restore(device, macfw::cmp::kIpcr0AddressLo, ipcr0);
        std::cout << "restore iPCR[0]: " << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
    }
    if (opConnected) {
        const auto kr = macfw::cmp::restore(device, macfw::cmp::kOpcr0AddressLo, opcr0);
        std::cout << "restore oPCR[0]: " << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
    }

    playback.release();
    capture.release();
    if (notifications) (*native)->TurnOffNotification(native);
    if (isochDispatcher) (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (callbackDispatcher) (*native)->RemoveCallbackDispatcherFromRunLoop(native);

    std::uint32_t opAfter = 0, ipAfter = 0;
    if (macfw::cmp::readOpcr0(device, opAfter) == kIOReturnSuccess &&
        macfw::cmp::readIpcr0(device, ipAfter) == kIOReturnSuccess) {
        std::cout << "post-test PCR restore: "
                  << ((opAfter == opcr0 && ipAfter == ipcr0) ? "PASS" : "FAIL") << '\n';
    }
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else {
            std::cerr << "usage: ./pcmbufferplayback [--execute]\n";
            return 64;
        }
    }

    std::cout << "macfw pcmbufferplayback — direct FW410 PCM-buffer playback test\n\n";
    return run(execute) ? 0 : 1;
}
