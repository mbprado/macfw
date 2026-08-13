#include "macfw/am824.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <CoreFoundation/CoreFoundation.h>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

constexpr UInt32 kCaptureMaxPacket48k = 168;
constexpr UInt32 kPlaybackMaxPacket48k = 360;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kPlaybackSlots = 128;
constexpr UInt32 kDefaultCycleLead = 256;

void dumpCapture(const macfw::AmdtpReceiveRing& ring, bool raw) {
    std::size_t touched = 0;
    std::size_t dataBearing = 0;
    macfw::am824::CaptureStats stats;

    for (std::size_t i = 0; i < ring.packetCount(); ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched())
            continue;
        ++touched;
        const auto packet = slot.packet();
        if (packet.length > 8) {
            ++dataBearing;
            macfw::am824::accumulateCapture48k(packet.payload, packet.length, stats);
        }
    }

    std::cout << "capture summary:\n";
    std::cout << "    touched slots:      " << touched << " / " << ring.packetCount() << '\n';
    std::cout << "    data-bearing slots: " << dataBearing << '\n';

    std::size_t shown = 0;
    for (std::size_t i = 0; i < ring.packetCount() && shown < 16; ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched())
            continue;

        const auto packet = slot.packet();
        const auto cip = packet.cip();
        const unsigned tag = (slot.isoHeader >> 14) & 0x3;
        const unsigned sy = slot.isoHeader & 0xf;
        std::cout << "    packet " << i
                  << ": len=" << packet.length
                  << " tag=" << tag
                  << " sy=" << sy;
        if (packet.hasCip()) {
            std::cout << " CIP{dbs=" << static_cast<unsigned>(cip.dbs)
                      << " dbc=" << static_cast<unsigned>(cip.dbc)
                      << " fmt=0x" << std::hex << static_cast<unsigned>(cip.fmt)
                      << " fdf=0x" << static_cast<unsigned>(cip.fdf)
                      << " syt=0x" << cip.syt << std::dec << "}";
        }
        std::cout << '\n';

        if (raw && packet.payload && packet.length) {
            const std::size_t n = packet.length < 48 ? packet.length : 48;
            std::cout << "        raw:";
            for (std::size_t j = 0; j < n; ++j)
                std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned>(packet.payload[j]);
            std::cout << std::dec << std::setfill(' ') << '\n';
        }
        ++shown;
    }

    if (dataBearing)
        std::cout << "result: PASS - FW410 transitioned to sample-bearing capture packets\n\n";
    else if (touched)
        std::cout << "result: PARTIAL - duplex ISO traffic active, capture still NODATA\n\n";
    else
        std::cout << "result: FAIL - no capture ISO packets observed\n\n";

    macfw::am824::printCaptureStats(stats, std::cout);
}

bool run(bool execute, bool raw, UInt32 cycleLead) {
    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) {
        std::cout << "No operational FW 410 unit found.\n";
        return false;
    }

    std::cout << "FW410 operational unit:\n";
    std::cout << "    generation: " << device.generation() << '\n';
    std::cout << "    remote node: 0x" << std::hex << device.nodeID() << std::dec << '\n';

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

    std::cout << "preflight (48 kHz reusable callback-free playback):\n";
    std::cout << "    oPCR[0]: 0x" << std::hex << opcr0 << std::dec << '\n';
    std::cout << "    iPCR[0]: 0x" << std::hex << ipcr0 << std::dec << '\n';
    std::cout << "    capture max packet:  " << kCaptureMaxPacket48k << " bytes\n";
    std::cout << "    playback max packet: " << kPlaybackMaxPacket48k << " bytes\n";
    std::cout << "    playback: reusable 128-cycle AM824 silence ring\n";
    std::cout << "    start lead: " << cycleLead << " cycles\n";

    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n";
        return false;
    }

    auto native = device.nativeHandle();
    UInt32 cycleTime = 0;
    if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) {
        std::cout << "GetCycleTime failed\n";
        return false;
    }
    const UInt32 now = (cycleTime >> 12) & 0x1fffu;
    const UInt32 firstCycle = (now + cycleLead) & 0x1fffu;

    if (!execute) {
        std::cout << "TX plan (dry run):\n";
        std::cout << "    cycle timer: 0x" << std::hex << cycleTime << std::dec << '\n';
        std::cout << "    current cycle: " << now << '\n';
        std::cout << "    planned first TX cycle: " << firstCycle << '\n';
        macfw::am824::Playback48kState state{};
        for (std::size_t i = 0; i < 16; ++i) {
            const UInt32 cycle = (firstCycle + static_cast<UInt32>(i)) & 0x1fffu;
            const auto p = macfw::am824::buildPlayback48kSilence(cycle, state);
            std::cout << "    packet " << i << ": cycle=" << cycle
                      << " len=" << p.length
                      << " dbc=" << static_cast<unsigned>(p.dbc)
                      << " syt=0x" << std::hex << p.syt << std::dec
                      << (p.dataBearing ? " SILENCE" : " NODATA") << '\n';
            if (p.dataBearing)
                state.dbc = static_cast<std::uint8_t>(state.dbc + 8u);
            state.phase = static_cast<std::uint8_t>((state.phase + 1u) & 3u);
        }
        std::cout << "status: PASS - dry run only; nothing transmitted\n";
        std::cout << "to execute: ./isoplayback --execute [--raw] [--cycle-lead N]\n";
        return true;
    }

    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket48k);
    auto tx = macfw::AmdtpTransmitRing::createSilence48k(device, firstCycle, kPlaybackSlots);
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
    (*playback.nativeChannel())->SetTalker(
        playback.nativeChannel(),
        reinterpret_cast<IOFireWireLibIsochPortRef>(tx.nativeLocalPort()));

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
    if ((*native)->TurnOnNotification(native))
        notifications = true;

    if (capture.allocate() != kIOReturnSuccess || playback.allocate() != kIOReturnSuccess) {
        std::cout << "ISO resource allocation failed\n";
        goto cleanup;
    }

    std::cout << "ISO resources:\n";
    std::cout << "    capture channel:  " << capture.channel() << '\n';
    std::cout << "    playback channel: " << playback.channel() << '\n';

    if (macfw::cmp::connectOpcr0(device, opcr0, capture.channel(), capture.speed()) != kIOReturnSuccess)
        goto cleanup;
    opConnected = true;
    if (macfw::cmp::connectIpcr0(device, ipcr0, playback.channel()) != kIOReturnSuccess)
        goto cleanup;
    ipConnected = true;

    // Match the proven/Linux ordering: host playback first, then capture.
    if ((*playback.nativeChannel())->Start(playback.nativeChannel()) != kIOReturnSuccess) {
        std::cout << "playback start failed\n";
        goto cleanup;
    }
    playbackStarted = true;

    if ((*capture.nativeChannel())->Start(capture.nativeChannel()) != kIOReturnSuccess) {
        std::cout << "capture start failed\n";
        goto cleanup;
    }
    captureStarted = true;

    std::cout << "duplex ISO: started (reusable prebuilt timed PCM silence)\n";
    std::cout << "observation window: 2.0 s\n";
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false);

    dumpCapture(rx, raw);
    std::cout << "\nplayback TX mode:\n";
    std::cout << "    callback dependency: none\n";
    std::cout << "    dynamic updates:     none\n";
    std::cout << "    ring packets:        " << tx.packetCount() << '\n';
    ok = true;

cleanup:
    if (playbackStarted)
        (*playback.nativeChannel())->Stop(playback.nativeChannel());
    if (captureStarted)
        (*capture.nativeChannel())->Stop(capture.nativeChannel());

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

    if (notifications)
        (*native)->TurnOffNotification(native);
    if (isochDispatcher)
        (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (callbackDispatcher)
        (*native)->RemoveCallbackDispatcherFromRunLoop(native);

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
    bool raw = false;
    UInt32 cycleLead = kDefaultCycleLead;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute")
            execute = true;
        else if (arg == "--raw")
            raw = true;
        else if (arg == "--cycle-lead" && i + 1 < argc) {
            try {
                cycleLead = static_cast<UInt32>(std::stoul(argv[++i]));
            } catch (...) {
                std::cerr << "invalid --cycle-lead value\n";
                return 64;
            }
            if (cycleLead == 0 || cycleLead >= 8192) {
                std::cerr << "--cycle-lead must be 1..8191\n";
                return 64;
            }
        } else {
            std::cerr << "usage: ./isoplayback [--execute] [--raw] [--cycle-lead N]\n";
            return 64;
        }
    }

    std::cout << "macfw isoplayback — reusable callback-free FW410 playback test\n\n";
    return run(execute, raw, cycleLead) ? 0 : 1;
}
