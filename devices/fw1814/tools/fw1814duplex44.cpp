#include "../fcp_control.h"
#include "../transport/blocking_pcm_tx44.h"
#include "../transport/duplex_lifecycle.h"
#include "../transport/realtime_service.h"

#include "macfw/amdtp_receive_ring.h"
#include "macfw/firewire_device.h"
#include "macfw/pcm_ring_buffer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

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
constexpr std::size_t kPcmCapacityFrames = 16384;
constexpr std::size_t kCaptureSnapshots = 8;
constexpr double kCaptureSnapshotSeconds = 0.250;

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
        // Keep the general callback dispatcher alive while servicing the live
        // 44.1-kHz packet continuation. Isoch callbacks run on their dedicated
        // user-interactive run-loop thread.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.00025, false);

        UInt32 cycleTime = 0;
        if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess)
            return false;
        streamer.service(cycleCount(cycleTime));
    }
    return true;
}

struct CaptureSnapshot {
    std::size_t touched = 0;
    std::size_t data = 0;
    std::size_t nodata = 0;
    std::size_t bad = 0;
};

void printCip(const macfw::amdtp::CipHeader& cip) {
    std::cout << "CIP{sid=" << static_cast<unsigned>(cip.sid)
              << " dbs=" << static_cast<unsigned>(cip.dbs)
              << " dbc=" << static_cast<unsigned>(cip.dbc)
              << " fmt=0x" << std::hex << static_cast<unsigned>(cip.fmt)
              << " fdf=0x" << static_cast<unsigned>(cip.fdf)
              << " syt=0x" << cip.syt << std::dec << '}';
}

CaptureSnapshot inspectCapture(const macfw::AmdtpReceiveRing& rx,
                               bool raw,
                               bool showNormalPackets) {
    CaptureSnapshot result{};
    std::size_t shown = 0;

    for (std::size_t i = 0; i < rx.packetCount(); ++i) {
        const auto& slot = rx.slot(i);
        if (!slot.touched()) continue;
        ++result.touched;

        const auto packet = slot.packet();
        bool valid = false;
        bool isData = false;
        bool isNoData = false;
        bool hasCip = packet.hasCip();
        macfw::amdtp::CipHeader cip{};
        if (hasCip) {
            cip = packet.cip();
            if (slot.packetLength() == 8 && cip.syt == 0xffffu &&
                cip.fmt == 0x10 && cip.fdf == 0x01) {
                valid = true;
                isNoData = true;
            } else if (slot.packetLength() == kCaptureMaxPacket &&
                       cip.dbs == 11 && cip.fmt == 0x10 && cip.fdf == 0x01) {
                valid = true;
                isData = true;
            }
        }

        if (!valid) {
            ++result.bad;
            std::cout << "    INVALID packet " << i
                      << ": len=" << slot.packetLength()
                      << " isoHeader=0x" << std::hex << slot.isoHeader
                      << " status=0x" << slot.status
                      << " timestamp=0x" << slot.timestamp << std::dec;
            if (hasCip) {
                std::cout << ' ';
                printCip(cip);
            } else {
                std::cout << " no-CIP";
            }
            std::cout << '\n';
        } else if (raw && showNormalPackets && shown < 16) {
            std::cout << "    packet " << i
                      << ": len=" << slot.packetLength() << ' ';
            printCip(cip);
            std::cout << '\n';
            ++shown;
        }

        if (isData) ++result.data;
        if (isNoData) ++result.nodata;
    }

    return result;
}

bool run(bool execute, bool raw) {
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

    const UInt32 initialCycle = cycleCount(initialCycleTime);
    const UInt32 firstCycle = (initialCycle + kCycleLead) % kCyclesPerSecond;

    macfw::PcmRingBuffer pcm(kPcmCapacityFrames,
                             BlockingPcmTransmitRing44100::pcmChannels());
    auto rx = macfw::AmdtpReceiveRing::create(
        device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = BlockingPcmTransmitRing44100::create(
        device, firstCycle, kTxPackets);
    BlockingPcmStream44100 streamer(
        tx, pcm, initialCycle, firstCycle, kTxHalfPackets);

    if (!pcm.valid() || !rx || !tx || !streamer.valid() || !streamer.prime()) {
        std::cerr << "FW1814 44.1 transport ring setup/prime failed\n";
        device.close();
        return false;
    }

    std::cout << "FW1814 44.1 blocking transport preflight:\n"
              << "    playback: DBS=7, 6 PCM + 1 MIDI, FDF=0x01\n"
              << "    playback max packet: " << kPlaybackMaxPacket << " bytes\n"
              << "    playback TX ring: " << kTxPackets << " / "
              << kTxHalfPackets << " packets\n"
              << "    44.1 blocking ratio: 441 data packets / 640 bus cycles\n"
              << "    capture: DBS=11, 10 PCM + 1 MIDI, FDF=0x01\n"
              << "    capture max packet: " << kCaptureMaxPacket << " bytes\n"
              << "    capture RX ring: " << kCaptureSlots << " packets\n"
              << "    scheduled TX lead: " << kCycleLead << " cycles (256 ms)\n";

    if (!execute) {
        std::cout << "status: PASS - dry run only; no CMP/ISO or AV/C write requested\n";
        device.close();
        return true;
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
        const bool rateOk = fcp.readInputRate(currentRate, raw) &&
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

    std::cout << "FW1814 44.1 duplex ISO started: playback ch="
              << lifecycle.playbackChannel()
              << " capture ch=" << lifecycle.captureChannel() << '\n';

    // The released FW410 native-44.1 path proved that a long scheduled lead and
    // active servicing through the first TX cycle are required. Wait until that
    // scheduled cycle has passed, plus 20 ms, before the post-start rate kick.
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
        std::cout << "FW1814 servicing 44.1 TX for " << std::fixed
                  << std::setprecision(3) << waitSeconds
                  << " s through scheduled first-cycle start\n"
                  << std::defaultfloat;
        if (!serviceFor(native, streamer, waitSeconds))
            goto cleanup;
    }

    std::cout << "FW1814 special 44.1 stream kick: OUTPUT 44100 Hz\n";
    if (!fcp.setSignalRate(kRate, 0x18, raw)) {
        std::cerr << "FW1814 OUTPUT 44100 CONTROL failed\n";
        goto cleanup;
    }

    // FW1814-specific proven quirk: preserve 100 ms between OUTPUT and INPUT,
    // but unlike a sleep, keep rebuilding the dynamic 44.1 packet continuation.
    std::cout << "FW1814 special 44.1 stream kick: servicing TX for 100 ms before INPUT\n";
    if (!serviceFor(native, streamer, 0.100))
        goto cleanup;

    std::cout << "FW1814 special 44.1 stream kick: INPUT 44100 Hz\n";
    if (!fcp.setSignalRate(kRate, 0x19, raw)) {
        std::cerr << "FW1814 INPUT 44100 CONTROL failed\n";
        goto cleanup;
    }

    if (!serviceFor(native, streamer, 0.050))
        goto cleanup;

    {
        unsigned readback = 0;
        if (!fcp.readInputRate(readback, raw) || readback != kRate) {
            std::cerr << "FW1814 post-start INPUT rate readback failed: "
                      << readback << " Hz\n";
            goto cleanup;
        }
        std::cout << "FW1814 post-start INPUT rate readback: "
                  << readback << " Hz PASS\n";
    }

    std::cout << "FW1814 sampling native 44.1 capture state every 250 ms for 2.0 s\n";
    {
        bool captureAlwaysValid = true;
        CaptureSnapshot capture{};
        for (std::size_t sample = 0; sample < kCaptureSnapshots; ++sample) {
            if (!serviceFor(native, streamer, kCaptureSnapshotSeconds))
                goto cleanup;

            capture = inspectCapture(rx, raw, sample == 0);
            std::cout << "    snapshot " << (sample + 1) << '/' << kCaptureSnapshots
                      << ": touched=" << capture.touched << '/' << rx.packetCount()
                      << " data=" << capture.data
                      << " nodata=" << capture.nodata
                      << " invalid=" << capture.bad << '\n';

            if (capture.touched != rx.packetCount() || capture.data == 0 ||
                capture.nodata == 0 || capture.bad != 0)
                captureAlwaysValid = false;
        }

        const auto& txStats = streamer.stats();
        std::cout << "FW1814 44.1 transport result:\n"
                  << "    final capture touched: " << capture.touched << '/'
                  << rx.packetCount() << '\n'
                  << "    final capture data packets: " << capture.data << '\n'
                  << "    final capture NODATA packets: " << capture.nodata << '\n'
                  << "    final capture invalid packets: " << capture.bad << '\n'
                  << "    capture valid in all snapshots: "
                  << (captureAlwaysValid ? "yes" : "no") << '\n'
                  << "    TX halves refilled: " << txStats.halvesRefilled << '\n'
                  << "    TX data packets refilled: "
                  << txStats.dataPacketsRefilled << '\n'
                  << "    TX silence frames: " << txStats.framesSilenced << '\n'
                  << "    TX late cycle polls: " << txStats.lateCyclePolls
                  << " (diagnostic; synchronous FCP waits currently contribute)\n";

        // Non-zero lateCyclePolls are currently diagnostic only: the synchronous
        // FCP helper can pause streamer.service() for >32 cycles while waiting
        // for OUTPUT/INPUT/STATUS responses. A 320-cycle refill half still gives
        // ample margin. Capture validity and generation stability remain hard gates.
        ok = captureAlwaysValid && txStats.halvesRefilled != 0 &&
             lifecycle.generationStillValid();
        std::cout << "status: " << (ok ? "PASS" : "FAIL") << '\n';
    }

cleanup:
    // After any streaming FCP failure: no further AV/C. Stop local ISO and
    // restore PCR only when the original bus generation is still valid.
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
    bool execute = false;
    bool raw = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else if (arg == "--raw") raw = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: " << argv[0] << " [--execute] [--raw]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 64;
        }
    }

    std::cout << "macfw fw1814duplex44 — guarded native 44.1 kHz blocking duplex probe\n";
    if (!execute)
        std::cout << "dry-run mode: no CMP/ISO or AV/C writes will be performed\n";
    return run(execute, raw) ? 0 : 1;
}
