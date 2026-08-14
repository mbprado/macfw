#include "macfw/amdtp_pcm_stream.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"
#include "macfw/pcm_ring_buffer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr UInt32 kCaptureMaxPacket48k = 168;
constexpr UInt32 kPlaybackMaxPacket48k = 360;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kPlaybackSlots = 128;
constexpr std::size_t kHalfPackets = kPlaybackSlots / 2;
constexpr std::size_t kPcmChannels = 10;
constexpr std::size_t kPcmCapacityFrames = 4096;
constexpr UInt32 kCycleLead = 256;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr double kRunSeconds = 4.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kAmplitude = 131072.0;
constexpr std::uint64_t kToneSegmentFrames = 24000; // 0.5 s at 48 kHz.

UInt32 cycleCount(UInt32 cycleTime) { return (cycleTime >> 12) & 0x1fffu; }

struct Producer {
    std::uint64_t nextFrame = 0;

    std::size_t fill(macfw::PcmRingBuffer& ring) {
        const std::size_t free = ring.freeFrames();
        if (!free) return 0;
        const std::size_t chunk = std::min<std::size_t>(free, 1024);
        std::vector<std::int32_t> pcm(chunk * kPcmChannels, 0);
        for (std::size_t i = 0; i < chunk; ++i) {
            const std::uint64_t frame = nextFrame + i;
            const bool high = ((frame / kToneSegmentFrames) & 1u) != 0;
            const double hz = high ? 880.0 : 440.0;
            const double phase = 2.0 * kPi * hz * static_cast<double>(frame) / 48000.0;
            // FW410 physical Analog Output 1 = stream PCM position 2 (index 1).
            pcm[i * kPcmChannels + 1] =
                static_cast<std::int32_t>(std::sin(phase) * kAmplitude);
        }
        const std::size_t written = ring.write(pcm.data(), chunk);
        nextFrame += written;
        return written;
    }
};

void fillProducerAhead(macfw::PcmRingBuffer& ring, Producer& producer) {
    while (ring.freeFrames() >= 256) {
        if (!producer.fill(ring)) break;
    }
}

void producerLoop(macfw::PcmRingBuffer& ring,
                  Producer& producer,
                  std::atomic<bool>& stop,
                  std::atomic<std::uint64_t>& fillCalls,
                  std::atomic<std::uint64_t>& idleSleeps) {
    while (!stop.load(std::memory_order_acquire)) {
        const std::size_t written = producer.fill(ring);
        if (written) {
            fillCalls.fetch_add(1, std::memory_order_relaxed);
        } else {
            idleSleeps.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void dumpCapture(const macfw::AmdtpReceiveRing& ring) {
    std::size_t touched = 0, dataBearing = 0;
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
    if (!device) { std::cout << "No operational FW 410 unit found.\n"; return false; }

    std::cout << "FW410 operational unit:\n"
              << "    generation: " << device.generation() << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID() << std::dec << '\n';
    if (device.open() != kIOReturnSuccess) { std::cout << "open failed\n"; return false; }

    std::uint32_t opcr0 = 0, ipcr0 = 0;
    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) {
        std::cout << "PCR read failed\n"; return false;
    }
    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n"; return false;
    }

    auto native = device.nativeHandle();
    UInt32 cycleTime = 0;
    if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) return false;
    const UInt32 initialCycle = cycleCount(cycleTime);
    const UInt32 firstCycle = (initialCycle + kCycleLead) % kCyclesPerSecond;

    std::cout << "live PCM streaming experiment:\n"
              << "    sample rate:        48000 Hz\n"
              << "    stream positions:   10\n"
              << "    PCM FIFO:           " << kPcmCapacityFrames << " frames\n"
              << "    TX ring:            128 packets, two 64-packet refill halves\n"
              << "    scheduler:          reusable AmdtpPcmStream48k\n"
              << "    PCM producer:       independent std::thread\n"
              << "    cycle source:       host polls FireWire cycle timer\n"
              << "    payload update:     mmap data only; DCL metadata unchanged\n"
              << "    Analog Output 1:    alternating 440 / 880 Hz every 0.5 s\n"
              << "    observation window: " << kRunSeconds << " s\n";

    if (!execute) {
        std::cout << "status: PASS - dry run only; no stream started\n"
                  << "to execute: ./pcmstreamplayback --execute\n";
        return true;
    }

    macfw::PcmRingBuffer pcm(kPcmCapacityFrames, kPcmChannels);
    Producer producer;
    // Prime synchronously so the transport can start with a full source FIFO.
    fillProducerAhead(pcm, producer);

    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket48k);
    auto tx = macfw::AmdtpTransmitRing::createSilence48k(device, firstCycle, kPlaybackSlots);
    auto capture = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::DeviceToHost, kCaptureMaxPacket48k);
    auto playback = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::HostToDevice, kPlaybackMaxPacket48k);
    if (!rx || !tx || !capture || !playback || !pcm.valid()) {
        std::cout << "transport object creation failed\n"; return false;
    }

    macfw::AmdtpPcmStream48k streamer(tx, pcm, initialCycle, firstCycle, kHalfPackets);
    if (!streamer.valid() || !streamer.prime()) {
        std::cout << "TX stream scheduler prime failed\n"; return false;
    }
    fillProducerAhead(pcm, producer);

    (*capture.nativeChannel())->AddListener(
        capture.nativeChannel(), reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
    if (playback.bindHostToDeviceTalkerFirst(tx.nativeLocalPort()) != kIOReturnSuccess) {
        std::cout << "playback port binding failed\n"; return false;
    }

    bool callbackDispatcher = false, isochDispatcher = false, notifications = false;
    bool captureStarted = false, playbackStarted = false, opConnected = false, ipConnected = false;
    bool ok = false;
    std::atomic<bool> producerStop{false};
    std::atomic<std::uint64_t> producerFillCalls{0};
    std::atomic<std::uint64_t> producerIdleSleeps{0};
    std::thread producerThread;

    if ((*native)->AddCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess)
        callbackDispatcher = true;
    if ((*native)->AddIsochCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess)
        isochDispatcher = true;
    if ((*native)->TurnOnNotification(native)) notifications = true;

    if (capture.allocate() != kIOReturnSuccess || playback.allocate() != kIOReturnSuccess) {
        std::cout << "ISO resource allocation failed\n"; goto cleanup;
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
    if ((*playback.nativeChannel())->Start(playback.nativeChannel()) != kIOReturnSuccess) goto cleanup;
    playbackStarted = true;
    if ((*capture.nativeChannel())->Start(capture.nativeChannel()) != kIOReturnSuccess) goto cleanup;
    captureStarted = true;

    producerThread = std::thread(producerLoop, std::ref(pcm), std::ref(producer),
                                 std::ref(producerStop), std::ref(producerFillCalls),
                                 std::ref(producerIdleSleeps));

    std::cout << "duplex ISO: started (asynchronous PCM producer + library-managed refill)\n"
              << "listen for Output 1 to alternate low/high pitch every 0.5 s\n";

    {
        const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kRunSeconds;
        while (CFAbsoluteTimeGetCurrent() < deadline) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, false);

            UInt32 ct = 0;
            if ((*native)->GetCycleTime(native, &ct) != kIOReturnSuccess) continue;
            streamer.service(cycleCount(ct));
        }
    }

    producerStop.store(true, std::memory_order_release);
    if (producerThread.joinable()) producerThread.join();

    dumpCapture(rx);
    {
        const auto& stats = streamer.stats();
        std::cout << "streaming statistics:\n"
                  << "    PCM produced frames: " << pcm.producedFrames() << '\n'
                  << "    PCM consumed frames: " << pcm.consumedFrames() << '\n'
                  << "    TX halves refilled:   " << stats.halvesRefilled << '\n'
                  << "    TX data packets:      " << stats.dataPacketsRefilled << '\n'
                  << "    refill PCM frames:    " << stats.framesFromBuffer << '\n'
                  << "    refill silence frames:" << stats.framesSilenced << '\n'
                  << "    PCM underrun frames:  " << pcm.underrunFrames() << '\n'
                  << "    late cycle polls:     " << stats.lateCyclePolls << '\n'
                  << "    producer fill calls:  " << producerFillCalls.load() << '\n'
                  << "    producer idle sleeps: " << producerIdleSleeps.load() << '\n';
    }
    ok = true;

cleanup:
    producerStop.store(true, std::memory_order_release);
    if (producerThread.joinable()) producerThread.join();

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
    playback.release(); capture.release();
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
        else { std::cerr << "usage: ./pcmstreamplayback [--execute]\n"; return 64; }
    }
    std::cout << "macfw pcmstreamplayback — asynchronous live FW410 PCM streaming test\n\n";
    return run(execute) ? 0 : 1;
}
