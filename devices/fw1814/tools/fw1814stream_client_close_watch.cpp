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
constexpr UInt32 kCaptureMaxPayload = 360;
constexpr UInt32 kPlaybackMaxPayload = 232;
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

bool reportGeneration(IOFireWireLibDeviceRef observer,
                      UInt32 baseline,
                      const char* label,
                      bool& changed,
                      std::string& firstChange) {
    UInt32 observed = 0;
    if (!readGeneration(observer, observed)) {
        std::cout << "    " << label << ": generation read FAILED\n";
        return false;
    }

    std::cout << "    " << label << ": generation " << observed;
    if (observed != baseline) {
        std::cout << "  <-- CHANGED from " << baseline;
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

void waitAndReport(IOFireWireLibDeviceRef observer,
                   UInt32 baseline,
                   double seconds,
                   const char* label,
                   bool& changed,
                   std::string& firstChange) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    reportGeneration(observer, baseline, label, changed, firstChange);
}

bool run(bool execute) {
    // Keep an unopened observer handle alive while a second client owns and
    // tears down the complete ISO transport. The observer issues no writes.
    auto observer = macfw::FireWireDevice::findByProductName(kProduct);
    if (!observer) {
        std::cout << "No operational FW 1814 observer handle found.\n";
        return false;
    }
    auto observerNative = observer.nativeHandle();

    UInt32 baseline = 0;
    if (!readGeneration(observerNative, baseline)) {
        std::cout << "observer generation read failed\n";
        return false;
    }

    auto client = macfw::FireWireDevice::findByProductName(kProduct);
    if (!client) {
        std::cout << "No operational FW 1814 stream-client handle found.\n";
        return false;
    }

    std::cout << "FW1814 streamed-client final-lifecycle generation watch:\n"
              << "    baseline generation: " << baseline << '\n'
              << "    observer node: 0x" << std::hex << observer.nodeID()
              << std::dec << '\n'
              << "    blocking transport: playback DBS=7, 6 PCM + 1 MIDI, 8/8/8/NODATA\n"
              << "    capture reservation: 360 bytes\n"
              << "    AV/C rate CONTROL: NONE\n"
              << "    prerequisite: device already initialized to 48 kHz\n";

    if (!execute) {
        std::cout << "status: PASS - dry run only\n";
        return true;
    }

    const IOReturn openKr = client.open();
    std::cout << "stream client Open: 0x" << std::hex << openKr
              << std::dec << '\n';
    if (openKr != kIOReturnSuccess)
        return false;

    auto native = client.nativeHandle();
    bool changed = false;
    std::string firstChange;
    bool isochDispatcher = false;
    bool notifications = false;
    bool opcrConnected = false;
    bool ipcrConnected = false;
    bool playbackStarted = false;
    bool captureStarted = false;
    bool setupOk = true;

    std::uint32_t opcr0 = 0;
    std::uint32_t ipcr0 = 0;

    auto receiveRing = macfw::AmdtpReceiveRing{};
    auto transmitRing = macfw::fw1814::BlockingSilenceTransmitRing{};
    auto capture = macfw::IsochAllocation{};
    auto playback = macfw::IsochAllocation{};
    IOFireWireLibIsochChannelRef captureChannel = nullptr;
    IOFireWireLibIsochChannelRef playbackChannel = nullptr;

    do {
        if (macfw::cmp::readOpcr0(client, opcr0) != kIOReturnSuccess ||
            macfw::cmp::readIpcr0(client, ipcr0) != kIOReturnSuccess) {
            std::cout << "PCR read failed\n";
            setupOk = false;
            break;
        }

        const auto op = macfw::cmp::decodePcr(opcr0);
        const auto ip = macfw::cmp::decodePcr(ipcr0);
        std::cout << "preflight PCRs:\n"
                  << "    oPCR[0]: 0x" << std::hex << opcr0 << std::dec
                  << " online=" << (op.online ? "yes" : "no")
                  << " p2p=" << static_cast<unsigned>(op.p2pConnections) << '\n'
                  << "    iPCR[0]: 0x" << std::hex << ipcr0 << std::dec
                  << " online=" << (ip.online ? "yes" : "no")
                  << " p2p=" << static_cast<unsigned>(ip.p2pConnections) << '\n';
        if (!macfw::cmp::ready(op) || !macfw::cmp::ready(ip)) {
            std::cout << "PCR0 offline or already connected\n";
            setupOk = false;
            break;
        }

        UInt32 cycleTime = 0;
        if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) {
            std::cout << "GetCycleTime failed\n";
            setupOk = false;
            break;
        }
        const UInt32 currentCycle = (cycleTime >> 12) & 0x1fffu;
        const UInt32 firstTxCycle =
            (currentCycle + kTxCycleLead) % kCyclesPerSecond;

        receiveRing = macfw::AmdtpReceiveRing::create(
            client, kCapturePackets, kCaptureMaxPayload);
        transmitRing = macfw::fw1814::BlockingSilenceTransmitRing::create48k(
            client, firstTxCycle, kPlaybackDbs,
            kPlaybackPcmChannels, kTxPackets);
        capture = macfw::IsochAllocation::create(
            client, macfw::IsochAllocation::Direction::DeviceToHost,
            kCaptureMaxPayload);
        playback = macfw::IsochAllocation::create(
            client, macfw::IsochAllocation::Direction::HostToDevice,
            kPlaybackMaxPayload);

        if (!receiveRing || !transmitRing || !capture || !playback) {
            std::cout << "ISO resource creation failed\n";
            setupOk = false;
            break;
        }

        captureChannel = capture.nativeChannel();
        playbackChannel = playback.nativeChannel();
        if (!captureChannel || !playbackChannel) {
            std::cout << "ISO channel handle missing\n";
            setupOk = false;
            break;
        }

        IOReturn kr = (*captureChannel)->AddListener(
            captureChannel,
            reinterpret_cast<IOFireWireLibIsochPortRef>(
                receiveRing.nativeLocalPort()));
        if (kr != kIOReturnSuccess) {
            std::cout << "capture AddListener failed: 0x" << std::hex << kr
                      << std::dec << '\n';
            setupOk = false;
            break;
        }

        kr = playback.bindHostToDeviceTalkerFirst(
            transmitRing.nativeLocalPort());
        if (kr != kIOReturnSuccess) {
            std::cout << "playback SetTalker failed: 0x" << std::hex << kr
                      << std::dec << '\n';
            setupOk = false;
            break;
        }

        if ((*native)->AddIsochCallbackDispatcherToRunLoop(
                native, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
            std::cout << "isoch callback dispatcher setup failed\n";
            setupOk = false;
            break;
        }
        isochDispatcher = true;
        if ((*native)->TurnOnNotification(native))
            notifications = true;

        if (playback.allocate() != kIOReturnSuccess ||
            capture.allocate() != kIOReturnSuccess) {
            std::cout << "ISO allocation failed\n";
            setupOk = false;
            break;
        }

        if (macfw::cmp::connectOpcr0(
                client, opcr0, capture.channel(), capture.speed()) !=
            kIOReturnSuccess) {
            std::cout << "connect oPCR[0] failed\n";
            setupOk = false;
            break;
        }
        opcrConnected = true;

        if (macfw::cmp::connectIpcr0(
                client, ipcr0, playback.channel()) != kIOReturnSuccess) {
            std::cout << "connect iPCR[0] failed\n";
            setupOk = false;
            break;
        }
        ipcrConnected = true;

        if ((*playbackChannel)->Start(playbackChannel) != kIOReturnSuccess) {
            std::cout << "playback start failed\n";
            setupOk = false;
            break;
        }
        playbackStarted = true;

        if ((*captureChannel)->Start(captureChannel) != kIOReturnSuccess) {
            std::cout << "capture start failed\n";
            setupOk = false;
            break;
        }
        captureStarted = true;

        std::cout << "both CMP connections + blocking TX/RX started\n";
        waitAndReport(observerNative, baseline, 0.30,
                      "300 ms with transport running",
                      changed, firstChange);
    } while (false);

    // Full remote/local ISO teardown. This portion was already observed stable
    // in teardown-watch, but we repeat it while the independent observer stays
    // alive so the final client lifecycle can be measured too.
    std::cout << "stream teardown:\n";

    if (captureStarted && captureChannel) {
        (*captureChannel)->Stop(captureChannel);
        captureStarted = false;
    }
    reportGeneration(observerNative, baseline,
                     "after capture Stop", changed, firstChange);

    if (playbackStarted && playbackChannel) {
        (*playbackChannel)->Stop(playbackChannel);
        playbackStarted = false;
    }
    reportGeneration(observerNative, baseline,
                     "after playback Stop", changed, firstChange);

    if (!changed && ipcrConnected) {
        const IOReturn kr = macfw::cmp::restore(
            client, macfw::cmp::kIpcr0AddressLo, ipcr0);
        std::cout << "    restore iPCR[0]: "
                  << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
        if (kr == kIOReturnSuccess)
            ipcrConnected = false;
        reportGeneration(observerNative, baseline,
                         "after iPCR restore", changed, firstChange);
    }

    if (!changed && opcrConnected) {
        const IOReturn kr = macfw::cmp::restore(
            client, macfw::cmp::kOpcr0AddressLo, opcr0);
        std::cout << "    restore oPCR[0]: "
                  << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
        if (kr == kIOReturnSuccess)
            opcrConnected = false;
        reportGeneration(observerNative, baseline,
                         "after oPCR restore", changed, firstChange);
    }

    if (!changed) {
        const IOReturn kr = capture.release();
        std::cout << "    capture ReleaseChannel: 0x" << std::hex << kr
                  << std::dec << '\n';
        reportGeneration(observerNative, baseline,
                         "after capture ReleaseChannel", changed, firstChange);
    }

    if (!changed) {
        const IOReturn kr = playback.release();
        std::cout << "    playback ReleaseChannel: 0x" << std::hex << kr
                  << std::dec << '\n';
        reportGeneration(observerNative, baseline,
                         "after playback ReleaseChannel", changed, firstChange);
    }

    capture = macfw::IsochAllocation{};
    playback = macfw::IsochAllocation{};
    receiveRing = macfw::AmdtpReceiveRing{};
    transmitRing = macfw::fw1814::BlockingSilenceTransmitRing{};
    reportGeneration(observerNative, baseline,
                     "after all ISO objects destroyed", changed, firstChange);

    std::cout << "final stream-client lifecycle:\n";

    if (notifications) {
        (*native)->TurnOffNotification(native);
        notifications = false;
    }
    reportGeneration(observerNative, baseline,
                     "after TurnOffNotification", changed, firstChange);

    if (isochDispatcher) {
        (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
        isochDispatcher = false;
    }
    reportGeneration(observerNative, baseline,
                     "after RemoveIsochCallbackDispatcher", changed, firstChange);

    std::cout << "closing streamed client...\n";
    client.close();
    reportGeneration(observerNative, baseline,
                     "immediately after streamed client Close",
                     changed, firstChange);
    waitAndReport(observerNative, baseline, 0.10,
                  "100 ms after streamed client Close",
                  changed, firstChange);

    std::cout << "destroying streamed IOFireWireDeviceInterface/plugin/service...\n";
    client = macfw::FireWireDevice{};
    reportGeneration(observerNative, baseline,
                     "immediately after streamed client interface release",
                     changed, firstChange);

    for (unsigned i = 1; i <= 50 && !changed; ++i) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
        UInt32 observed = 0;
        if (!readGeneration(observerNative, observed)) {
            std::cout << "    observer generation read FAILED at +"
                      << (i * 20) << " ms after release\n";
            break;
        }
        if (observed != baseline) {
            std::cout << "    generation changed by +" << (i * 20)
                      << " ms after streamed client release: "
                      << baseline << " -> " << observed << '\n';
            changed = true;
            firstChange = "after streamed client interface release";
            break;
        }
    }

    if (!changed) {
        reportGeneration(observerNative, baseline,
                         "1000 ms after streamed client interface release",
                         changed, firstChange);
    }

    if (changed) {
        std::cout << "result: BUS GENERATION CHANGED first at: "
                  << firstChange << '\n';
    } else {
        std::cout << "result: generation remained stable through streamed client close/release\n";
    }

    if (!setupOk)
        std::cout << "note: transport setup was incomplete; lifecycle trace still completed safely\n";

    std::cout << "status: PASS - streamed client lifecycle diagnostic completed\n";
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

    std::cout << "macfw fw1814stream-client-close-watch — streamed client final-lifecycle diagnostic\n\n";
    return run(execute) ? 0 : 1;
}
