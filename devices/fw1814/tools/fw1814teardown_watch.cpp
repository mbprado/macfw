#include "fw1814_blocking_tx.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

constexpr const char* kProduct = "FW 1814";
constexpr UInt32 kCaptureMaxPayload = 360;   // 8 + 8 * 11 * 4
constexpr UInt32 kPlaybackMaxPayload = 232;  // 8 + 8 *  7 * 4
constexpr std::uint8_t kPlaybackDbs = 7;
constexpr std::uint8_t kPlaybackPcmChannels = 6;
constexpr std::size_t kCapturePackets = 64;
constexpr std::size_t kTxPackets = 128;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr UInt32 kTxCycleLead = 256;

bool readGeneration(IOFireWireLibDeviceRef native, UInt32& generation) {
    generation = 0;
    return native &&
           (*native)->GetBusGeneration(native, &generation) == kIOReturnSuccess;
}

bool waitAndReportGeneration(IOFireWireLibDeviceRef native,
                             UInt32 expected,
                             const char* label,
                             bool& changed,
                             std::string& firstChange) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);

    UInt32 observed = 0;
    if (!readGeneration(native, observed)) {
        std::cout << "    " << label << ": generation read FAILED\n";
        return false;
    }

    std::cout << "    " << label << ": generation " << observed;
    if (observed != expected) {
        std::cout << "  <-- CHANGED from " << expected;
        if (!changed) {
            changed = true;
            firstChange = label;
        }
    } else {
        std::cout << "  (stable)";
    }
    std::cout << '\n';
    return true;
}

bool run(bool execute) {
    auto device = macfw::FireWireDevice::findByProductName(kProduct);
    if (!device) {
        std::cout << "No operational FW 1814 unit found.\n";
        return false;
    }
    if (device.open() != kIOReturnSuccess) {
        std::cout << "open failed\n";
        return false;
    }

    auto native = device.nativeHandle();
    UInt32 initialGeneration = 0;
    if (!readGeneration(native, initialGeneration)) {
        std::cout << "initial generation read failed\n";
        device.close();
        return false;
    }

    std::uint32_t opcr0 = 0;
    std::uint32_t ipcr0 = 0;
    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) {
        std::cout << "PCR read failed\n";
        device.close();
        return false;
    }

    const auto op = macfw::cmp::decodePcr(opcr0);
    const auto ip = macfw::cmp::decodePcr(ipcr0);

    std::cout << "matched operational unit:\n"
              << "    product: " << kProduct << '\n'
              << "    generation: " << initialGeneration << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID()
              << std::dec << '\n'
              << "teardown-generation preflight:\n"
              << "    oPCR[0]: 0x" << std::hex << opcr0 << std::dec
              << " online=" << (op.online ? "yes" : "no")
              << " p2p=" << static_cast<unsigned>(op.p2pConnections) << '\n'
              << "    iPCR[0]: 0x" << std::hex << ipcr0 << std::dec
              << " online=" << (ip.online ? "yes" : "no")
              << " p2p=" << static_cast<unsigned>(ip.p2pConnections) << '\n'
              << "    blocking playback: DBS=7, 6 PCM + 1 MIDI, 8/8/8/NODATA\n"
              << "    AV/C rate CONTROL: NONE in this diagnostic\n"
              << "    prerequisite: device already initialized to 48 kHz\n";

    if (!macfw::cmp::ready(op) || !macfw::cmp::ready(ip)) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n";
        device.close();
        return false;
    }

    if (!execute) {
        std::cout << "status: PASS - dry run only\n";
        device.close();
        return true;
    }

    UInt32 cycleTime = 0;
    if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) {
        std::cout << "GetCycleTime failed\n";
        device.close();
        return false;
    }
    const UInt32 currentCycle = (cycleTime >> 12) & 0x1fffu;
    const UInt32 firstTxCycle =
        (currentCycle + kTxCycleLead) % kCyclesPerSecond;

    auto receiveRing = macfw::AmdtpReceiveRing::create(
        device, kCapturePackets, kCaptureMaxPayload);
    auto transmitRing = macfw::fw1814::BlockingSilenceTransmitRing::create48k(
        device, firstTxCycle, kPlaybackDbs, kPlaybackPcmChannels, kTxPackets);
    auto capture = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::DeviceToHost,
        kCaptureMaxPayload);
    auto playback = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::HostToDevice,
        kPlaybackMaxPayload);

    if (!receiveRing || !transmitRing || !capture || !playback) {
        std::cout << "ISO resource creation failed\n";
        device.close();
        return false;
    }

    auto captureChannel = capture.nativeChannel();
    auto playbackChannel = playback.nativeChannel();
    if (!captureChannel || !playbackChannel) {
        std::cout << "ISO channel handle missing\n";
        device.close();
        return false;
    }

    IOReturn kr = (*captureChannel)->AddListener(
        captureChannel,
        reinterpret_cast<IOFireWireLibIsochPortRef>(receiveRing.nativeLocalPort()));
    if (kr != kIOReturnSuccess) {
        std::cout << "capture AddListener failed: 0x" << std::hex << kr
                  << std::dec << '\n';
        device.close();
        return false;
    }

    kr = playback.bindHostToDeviceTalkerFirst(transmitRing.nativeLocalPort());
    if (kr != kIOReturnSuccess) {
        std::cout << "playback SetTalker failed: 0x" << std::hex << kr
                  << std::dec << '\n';
        device.close();
        return false;
    }

    bool isochDispatcher = false;
    bool notifications = false;
    bool opcrConnected = false;
    bool ipcrConnected = false;
    bool playbackStarted = false;
    bool captureStarted = false;
    bool generationChanged = false;
    std::string firstChange;

    if ((*native)->AddIsochCallbackDispatcherToRunLoop(
            native, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
        std::cout << "isoch callback dispatcher setup failed\n";
        device.close();
        return false;
    }
    isochDispatcher = true;
    if ((*native)->TurnOnNotification(native))
        notifications = true;

    if (playback.allocate() != kIOReturnSuccess ||
        capture.allocate() != kIOReturnSuccess) {
        std::cout << "ISO allocation failed\n";
        goto local_cleanup;
    }

    if (macfw::cmp::connectOpcr0(
            device, opcr0, capture.channel(), capture.speed()) != kIOReturnSuccess) {
        std::cout << "connect oPCR[0] failed\n";
        goto local_cleanup;
    }
    opcrConnected = true;

    if (macfw::cmp::connectIpcr0(
            device, ipcr0, playback.channel()) != kIOReturnSuccess) {
        std::cout << "connect iPCR[0] failed\n";
        goto local_cleanup;
    }
    ipcrConnected = true;

    if ((*playbackChannel)->Start(playbackChannel) != kIOReturnSuccess) {
        std::cout << "playback start failed\n";
        goto local_cleanup;
    }
    playbackStarted = true;

    if ((*captureChannel)->Start(captureChannel) != kIOReturnSuccess) {
        std::cout << "capture start failed\n";
        goto local_cleanup;
    }
    captureStarted = true;

    std::cout << "both CMP connections + blocking TX/RX started\n";
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.30, false);
    if (!waitAndReportGeneration(native, initialGeneration,
                                 "before teardown", generationChanged,
                                 firstChange))
        goto local_cleanup;

    std::cout << "teardown generation trace:\n";

    (*captureChannel)->Stop(captureChannel);
    captureStarted = false;
    waitAndReportGeneration(native, initialGeneration,
                            "after capture Stop", generationChanged, firstChange);

    (*playbackChannel)->Stop(playbackChannel);
    playbackStarted = false;
    waitAndReportGeneration(native, initialGeneration,
                            "after playback Stop", generationChanged, firstChange);

    if (!generationChanged && ipcrConnected) {
        kr = macfw::cmp::restore(device, macfw::cmp::kIpcr0AddressLo, ipcr0);
        std::cout << "    restore iPCR[0]: "
                  << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
        if (kr == kIOReturnSuccess)
            ipcrConnected = false;
        waitAndReportGeneration(native, initialGeneration,
                                "after iPCR restore", generationChanged, firstChange);
    }

    if (!generationChanged && opcrConnected) {
        kr = macfw::cmp::restore(device, macfw::cmp::kOpcr0AddressLo, opcr0);
        std::cout << "    restore oPCR[0]: "
                  << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
        if (kr == kIOReturnSuccess)
            opcrConnected = false;
        waitAndReportGeneration(native, initialGeneration,
                                "after oPCR restore", generationChanged, firstChange);
    }

    if (!generationChanged) {
        kr = capture.release();
        std::cout << "    capture ReleaseChannel: 0x" << std::hex << kr
                  << std::dec << '\n';
        waitAndReportGeneration(native, initialGeneration,
                                "after capture ReleaseChannel",
                                generationChanged, firstChange);
    }

    if (!generationChanged) {
        kr = playback.release();
        std::cout << "    playback ReleaseChannel: 0x" << std::hex << kr
                  << std::dec << '\n';
        waitAndReportGeneration(native, initialGeneration,
                                "after playback ReleaseChannel",
                                generationChanged, firstChange);
    }

    if (!generationChanged) {
        capture = macfw::IsochAllocation{};
        waitAndReportGeneration(native, initialGeneration,
                                "after capture channel/remote-port destroy",
                                generationChanged, firstChange);
    }

    if (!generationChanged) {
        playback = macfw::IsochAllocation{};
        waitAndReportGeneration(native, initialGeneration,
                                "after playback channel/remote-port destroy",
                                generationChanged, firstChange);
    }

    if (!generationChanged) {
        receiveRing = macfw::AmdtpReceiveRing{};
        waitAndReportGeneration(native, initialGeneration,
                                "after receive local-port destroy",
                                generationChanged, firstChange);
    }

    if (!generationChanged) {
        transmitRing = macfw::fw1814::BlockingSilenceTransmitRing{};
        waitAndReportGeneration(native, initialGeneration,
                                "after transmit local-port destroy",
                                generationChanged, firstChange);
    }

    if (!generationChanged) {
        std::cout << "    waiting 500 ms after full teardown...\n";
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.50, false);
        waitAndReportGeneration(native, initialGeneration,
                                "500 ms after teardown",
                                generationChanged, firstChange);
    }

local_cleanup:
    if (captureStarted && captureChannel)
        (*captureChannel)->Stop(captureChannel);
    if (playbackStarted && playbackChannel)
        (*playbackChannel)->Stop(playbackChannel);

    // Never issue remote PCR writes after observing a generation change.
    if (!generationChanged) {
        if (ipcrConnected)
            macfw::cmp::restore(device, macfw::cmp::kIpcr0AddressLo, ipcr0);
        if (opcrConnected)
            macfw::cmp::restore(device, macfw::cmp::kOpcr0AddressLo, opcr0);
    }

    if (notifications)
        (*native)->TurnOffNotification(native);
    if (isochDispatcher)
        (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);

    device.close();

    if (generationChanged) {
        std::cout << "result: BUS GENERATION CHANGED first at: "
                  << firstChange << '\n';
    } else {
        std::cout << "result: generation remained stable through full teardown\n";
    }
    std::cout << "status: PASS - teardown diagnostic completed\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute")
            execute = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: " << argv[0] << " [--execute]\n";
            return 0;
        } else {
            std::cout << "usage: " << argv[0] << " [--execute]\n";
            return 64;
        }
    }

    std::cout << "macfw fw1814teardown-watch — stepwise bus-generation teardown diagnostic\n\n";
    return run(execute) ? 0 : 1;
}
