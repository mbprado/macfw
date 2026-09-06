#include "fw1814_tone_tx.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr const char* kProduct = "FW 1814";
constexpr UInt16 kAddressHi = 0xffff;
constexpr UInt32 kFcpCommandLo = 0xf0000b00;
constexpr UInt32 kFcpResponseLo = 0xf0000d00;
constexpr UInt32 kFcpResponseSize = 0x200;
constexpr double kFcpTimeoutSeconds = 1.0;
constexpr unsigned kRate = 48000;
constexpr UInt32 kCaptureMaxPayload = 360;
constexpr UInt32 kPlaybackMaxPayload = 232;
constexpr std::size_t kCapturePackets = 64;
constexpr std::size_t kTxPackets = 128;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr UInt32 kTxCycleLead = 256;
constexpr double kToneSeconds = 3.0;

struct ResponseContext {
    UInt16 expectedNode = 0;
    bool received = false;
    UInt32 length = 0;
    std::array<UInt8, kFcpResponseSize> bytes{};
};

UInt32 responseHandler(IOFireWireLibPseudoAddressSpaceRef space,
                       FWClientCommandID commandID,
                       UInt32 packetLen,
                       void* packet,
                       UInt16 srcNodeID,
                       UInt32,
                       UInt32,
                       void* refCon) {
    auto* ctx = static_cast<ResponseContext*>(refCon);
    if (ctx && packet && srcNodeID == ctx->expectedNode) {
        ctx->length = std::min<UInt32>(packetLen, ctx->bytes.size());
        std::memcpy(ctx->bytes.data(), packet, ctx->length);
        ctx->received = true;
    }
    (*space)->ClientCommandIsComplete(space, commandID, kIOReturnSuccess);
    return kIOReturnSuccess;
}

bool transaction(macfw::FireWireDevice& device,
                 ResponseContext& ctx,
                 const UInt8* command,
                 UInt32 length) {
    ctx.received = false;
    ctx.length = 0;
    ctx.bytes.fill(0);

    UInt32 size = length;
    const IOReturn kr = device.write(kAddressHi, kFcpCommandLo, command, size);
    if (kr != kIOReturnSuccess) {
        std::cout << "    FCP write failed: 0x" << std::hex << kr
                  << std::dec << '\n';
        return false;
    }

    const CFAbsoluteTime deadline =
        CFAbsoluteTimeGetCurrent() + kFcpTimeoutSeconds;
    while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);

    if (!ctx.received) {
        std::cout << "    FCP response timeout\n";
        return false;
    }
    return true;
}

bool validControlResponse(UInt8 response) {
    return response == 0x09 || response == 0x0c ||
           response == 0x0d || response == 0x0f;
}

bool setSignalRate(macfw::FireWireDevice& device,
                   ResponseContext& ctx,
                   UInt8 opcode) {
    const UInt8 command[8] = {
        0x00, 0xff, opcode, 0x00, 0x90, 0x02, 0xff, 0xff
    };
    if (!transaction(device, ctx, command,
                     static_cast<UInt32>(sizeof(command))))
        return false;
    if (ctx.length < 8 || !validControlResponse(ctx.bytes[0]))
        return false;
    return ctx.bytes[1] == 0xff &&
           ctx.bytes[2] == opcode &&
           ctx.bytes[3] == 0x00 &&
           ctx.bytes[4] == 0x90 &&
           (ctx.bytes[5] & 0x07) == 0x02;
}

bool readInputRate(macfw::FireWireDevice& device,
                   ResponseContext& ctx,
                   unsigned& rate) {
    const UInt8 command[8] = {
        0x01, 0xff, 0x19, 0x00, 0x90, 0xff, 0xff, 0xff
    };
    if (!transaction(device, ctx, command,
                     static_cast<UInt32>(sizeof(command))))
        return false;
    if (ctx.length < 8 ||
        (ctx.bytes[0] != 0x0c && ctx.bytes[0] != 0x0d) ||
        ctx.bytes[1] != 0xff || ctx.bytes[2] != 0x19 ||
        ctx.bytes[3] != 0x00 || ctx.bytes[4] != 0x90)
        return false;

    static constexpr unsigned rates[] = {
        32000, 44100, 48000, 88200, 96000, 176400, 192000, 0
    };
    rate = rates[ctx.bytes[5] & 0x07];
    return rate != 0;
}

bool readGeneration(IOFireWireLibDeviceRef native, UInt32& generation) {
    generation = 0;
    return native &&
           (*native)->GetBusGeneration(native, &generation) == kIOReturnSuccess;
}

bool run(unsigned position, bool execute) {
    if (position >= 6) {
        std::cout << "status: REFUSED - playback PCM position must be 0..5\n";
        return false;
    }

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
        std::cout << "generation read failed\n";
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

    std::cout << "FW1814 playback-position mapper:\n"
              << "    generation: " << initialGeneration << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID()
              << std::dec << '\n'
              << "    selected PCM position: " << position << " / 5\n"
              << "    tone: 500 Hz, -24 dBFS peak, 3 seconds\n"
              << "    playback formation: 6 PCM + 1 MIDI, DBS=7\n"
              << "    capture companion: 10 PCM + 1 MIDI, DBS=11\n"
              << "    oPCR[0]: 0x" << std::hex << opcr0 << std::dec
              << " online=" << (op.online ? "yes" : "no")
              << " p2p=" << static_cast<unsigned>(op.p2pConnections) << '\n'
              << "    iPCR[0]: 0x" << std::hex << ipcr0 << std::dec
              << " online=" << (ip.online ? "yes" : "no")
              << " p2p=" << static_cast<unsigned>(ip.p2pConnections) << '\n';

    if (!macfw::cmp::ready(op) || !macfw::cmp::ready(ip)) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n";
        device.close();
        return false;
    }

    if (!execute) {
        std::cout << "    AV/C rate CONTROL: only in --execute mode\n"
                  << "    no ISO/CMP/rate writes made\n"
                  << "status: PASS - dry run only\n";
        device.close();
        return true;
    }

    bool callbackDispatcher = false;
    bool isochDispatcher = false;
    bool notifications = false;
    bool responseNotification = false;
    bool opcrConnected = false;
    bool ipcrConnected = false;
    bool playbackStarted = false;
    bool captureStarted = false;
    bool generationChanged = false;
    bool kickOk = false;
    bool packetsReceived = false;
    bool restoreOk = true;

    ResponseContext ctx;
    ctx.expectedNode = device.nodeID();
    IOFireWireLibPseudoAddressSpaceRef responseSpace = nullptr;

    auto receiveRing = macfw::AmdtpReceiveRing{};
    auto transmitRing = macfw::fw1814::BlockingToneTransmitRing{};
    auto capture = macfw::IsochAllocation{};
    auto playback = macfw::IsochAllocation{};
    IOFireWireLibIsochChannelRef captureChannel = nullptr;
    IOFireWireLibIsochChannelRef playbackChannel = nullptr;

    if ((*native)->AddCallbackDispatcherToRunLoop(
            native, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
        std::cout << "callback dispatcher setup failed\n";
        goto cleanup;
    }
    callbackDispatcher = true;

    responseSpace = (*native)->CreateInitialUnitsPseudoAddressSpace(
        native, kFcpResponseLo, kFcpResponseSize, &ctx, 1024, nullptr,
        kFWAddressSpaceNoReadAccess | kFWAddressSpaceShareIfExists,
        CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
    if (!responseSpace) {
        std::cout << "FCP response address-space setup failed\n";
        goto cleanup;
    }
    (*responseSpace)->SetWriteHandler(responseSpace, responseHandler);
    if (!(*responseSpace)->TurnOnNotification(responseSpace)) {
        std::cout << "FCP response notification setup failed\n";
        goto cleanup;
    }
    responseNotification = true;

    {
        unsigned currentRate = 0;
        const bool rateOk = readInputRate(device, ctx, currentRate) &&
                            currentRate == kRate;
        std::cout << "    authoritative INPUT rate: " << currentRate
                  << " Hz -> " << (rateOk ? "PASS" : "FAIL") << '\n';
        if (!rateOk) {
            std::cout << "status: REFUSED - initialize device to 48 kHz first\n";
            goto cleanup;
        }
    }

    {
        UInt32 cycleTime = 0;
        if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) {
            std::cout << "GetCycleTime failed\n";
            goto cleanup;
        }
        const UInt32 currentCycle = (cycleTime >> 12) & 0x1fffu;
        const UInt32 firstTxCycle =
            (currentCycle + kTxCycleLead) % kCyclesPerSecond;

        receiveRing = macfw::AmdtpReceiveRing::create(
            device, kCapturePackets, kCaptureMaxPayload);
        transmitRing = macfw::fw1814::BlockingToneTransmitRing::create48k(
            device, firstTxCycle, static_cast<std::uint8_t>(position), kTxPackets);
        capture = macfw::IsochAllocation::create(
            device, macfw::IsochAllocation::Direction::DeviceToHost,
            kCaptureMaxPayload);
        playback = macfw::IsochAllocation::create(
            device, macfw::IsochAllocation::Direction::HostToDevice,
            kPlaybackMaxPayload);

        if (!receiveRing || !transmitRing || !capture || !playback) {
            std::cout << "ISO resource creation failed\n";
            goto cleanup;
        }

        captureChannel = capture.nativeChannel();
        playbackChannel = playback.nativeChannel();
        if (!captureChannel || !playbackChannel) {
            std::cout << "ISO channel handle missing\n";
            goto cleanup;
        }

        IOReturn kr = (*captureChannel)->AddListener(
            captureChannel,
            reinterpret_cast<IOFireWireLibIsochPortRef>(
                receiveRing.nativeLocalPort()));
        if (kr != kIOReturnSuccess) {
            std::cout << "capture AddListener failed: 0x" << std::hex << kr
                      << std::dec << '\n';
            goto cleanup;
        }

        kr = playback.bindHostToDeviceTalkerFirst(
            transmitRing.nativeLocalPort());
        if (kr != kIOReturnSuccess) {
            std::cout << "playback SetTalker failed: 0x" << std::hex << kr
                      << std::dec << '\n';
            goto cleanup;
        }

        if ((*native)->AddIsochCallbackDispatcherToRunLoop(
                native, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
            std::cout << "isoch callback dispatcher setup failed\n";
            goto cleanup;
        }
        isochDispatcher = true;
        if ((*native)->TurnOnNotification(native))
            notifications = true;

        if (playback.allocate() != kIOReturnSuccess ||
            capture.allocate() != kIOReturnSuccess) {
            std::cout << "ISO allocation failed\n";
            goto cleanup;
        }

        if (macfw::cmp::connectOpcr0(
                device, opcr0, capture.channel(), capture.speed()) !=
            kIOReturnSuccess) {
            std::cout << "connect oPCR[0] failed\n";
            goto cleanup;
        }
        opcrConnected = true;

        if (macfw::cmp::connectIpcr0(
                device, ipcr0, playback.channel()) != kIOReturnSuccess) {
            std::cout << "connect iPCR[0] failed\n";
            goto cleanup;
        }
        ipcrConnected = true;

        if ((*playbackChannel)->Start(playbackChannel) != kIOReturnSuccess) {
            std::cout << "playback start failed\n";
            goto cleanup;
        }
        playbackStarted = true;

        if ((*captureChannel)->Start(captureChannel) != kIOReturnSuccess) {
            std::cout << "capture start failed\n";
            goto cleanup;
        }
        captureStarted = true;

        std::cout << "    both CMP connections + blocking TX/RX started\n"
                  << "    ISO settle: 50 ms\n";
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);

        std::cout << "    reassert OUTPUT 48000 Hz...\n";
        const bool outputOk = setSignalRate(device, ctx, 0x18);
        std::cout << "        result: " << (outputOk ? "PASS" : "FAIL") << '\n';
        if (!outputOk) goto cleanup;

        std::cout << "    waiting 100 ms before INPUT rate CONTROL...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::cout << "    reassert INPUT 48000 Hz...\n";
        const bool inputOk = setSignalRate(device, ctx, 0x19);
        std::cout << "        result: " << (inputOk ? "PASS" : "FAIL") << '\n';
        if (!inputOk) goto cleanup;
        kickOk = true;

        std::cout << "\n*** TONE ACTIVE: playback PCM position " << position
                  << " — 500 Hz at -24 dBFS peak for 3 seconds ***\n";
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, kToneSeconds, false);
        packetsReceived = receiveRing.touchedCount() > 0;
        std::cout << "    companion capture packets: "
                  << receiveRing.touchedCount() << " / "
                  << receiveRing.packetCount() << '\n';
    }

cleanup:
    if (captureStarted && captureChannel) {
        (*captureChannel)->Stop(captureChannel);
        captureStarted = false;
    }
    if (playbackStarted && playbackChannel) {
        (*playbackChannel)->Stop(playbackChannel);
        playbackStarted = false;
    }

    {
        UInt32 currentGeneration = 0;
        if (!readGeneration(native, currentGeneration) ||
            currentGeneration != initialGeneration) {
            generationChanged = true;
            std::cout << "    generation changed or became unreadable; "
                         "skipping remote PCR restore writes\n";
        }
    }

    // Never issue remote PCR writes after a generation change. If the stream
    // kick failed but the generation is still valid, exact PCR restoration is
    // still safe and is the only remaining remote write performed here.
    if (!generationChanged) {
        if (ipcrConnected) {
            const IOReturn kr = macfw::cmp::restore(
                device, macfw::cmp::kIpcr0AddressLo, ipcr0);
            std::cout << "    restore iPCR[0]: "
                      << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
            restoreOk = restoreOk && kr == kIOReturnSuccess;
            ipcrConnected = false;
        }
        if (opcrConnected) {
            const IOReturn kr = macfw::cmp::restore(
                device, macfw::cmp::kOpcr0AddressLo, opcr0);
            std::cout << "    restore oPCR[0]: "
                      << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
            restoreOk = restoreOk && kr == kIOReturnSuccess;
            opcrConnected = false;
        }
    }

    capture.release();
    playback.release();
    capture = macfw::IsochAllocation{};
    playback = macfw::IsochAllocation{};
    receiveRing = macfw::AmdtpReceiveRing{};
    transmitRing = macfw::fw1814::BlockingToneTransmitRing{};

    if (notifications)
        (*native)->TurnOffNotification(native);
    if (isochDispatcher)
        (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (responseNotification && responseSpace)
        (*responseSpace)->TurnOffNotification(responseSpace);
    if (responseSpace)
        (*responseSpace)->Release(responseSpace);
    if (callbackDispatcher)
        (*native)->RemoveCallbackDispatcherFromRunLoop(native);

    const bool success = kickOk && packetsReceived && restoreOk &&
                         !generationChanged;
    std::cout << "status: " << (success ? "PASS" : "FAIL")
              << " - playback position mapping run completed\n";
    device.close();
    return success;
}

void usage(const char* argv0) {
    std::cout << "usage: " << argv0
              << " --position <0..5> [--execute]\n";
}

} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    bool havePosition = false;
    unsigned position = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") {
            execute = true;
        } else if (arg == "--position" && i + 1 < argc) {
            try {
                const unsigned long value = std::stoul(argv[++i]);
                if (value > 5) {
                    usage(argv[0]);
                    return 64;
                }
                position = static_cast<unsigned>(value);
                havePosition = true;
            } catch (...) {
                usage(argv[0]);
                return 64;
            }
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 64;
        }
    }

    if (!havePosition) {
        usage(argv[0]);
        return 64;
    }

    std::cout << "macfw fw1814playback-map — guarded analog playback position mapper\n\n";
    return run(position, execute) ? 0 : 1;
}
