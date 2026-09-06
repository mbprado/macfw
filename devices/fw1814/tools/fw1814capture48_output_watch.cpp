#include "fw1814_blocking_tx.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

constexpr const char* kProduct = "FW 1814";
constexpr UInt16 kAddressHi = 0xffff;
constexpr UInt32 kInfoLo = 0xc8020000;
constexpr UInt32 kFcpCommandLo = 0xf0000b00;
constexpr UInt32 kFcpResponseLo = 0xf0000d00;
constexpr UInt32 kFcpResponseSize = 0x200;
constexpr double kFcpTimeoutSeconds = 1.0;

constexpr unsigned kRate = 48000;
constexpr UInt32 kCaptureMaxPayload = 360;
constexpr UInt32 kPlaybackMaxPayload = 232;
constexpr std::uint8_t kPlaybackDbs = 7;
constexpr std::uint8_t kPlaybackPcmChannels = 6;
constexpr std::size_t kCapturePackets = 64;
constexpr std::size_t kTxPackets = 128;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr UInt32 kTxCycleLead = 256;

struct ResponseContext {
    UInt16 expectedNode = 0;
    bool received = false;
    UInt32 length = 0;
    std::array<UInt8, kFcpResponseSize> bytes{};
};

UInt32 le32(const UInt8* p) {
    return static_cast<UInt32>(p[0]) |
           (static_cast<UInt32>(p[1]) << 8) |
           (static_cast<UInt32>(p[2]) << 16) |
           (static_cast<UInt32>(p[3]) << 24);
}

std::string asciiField(const UInt8* p, std::size_t len) {
    std::string out;
    for (std::size_t i = 0; i < len && p[i]; ++i)
        out.push_back(static_cast<char>(p[i]));
    return out;
}

void printBytes(const UInt8* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (i) std::cout << ' ';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(p[i]);
    }
    std::cout << std::dec << std::setfill(' ');
}

bool validateInfo(macfw::FireWireDevice& device) {
    std::array<UInt8, 0x68> info{};
    UInt32 size = static_cast<UInt32>(info.size());
    if (device.read(kAddressHi, kInfoLo, info.data(), size) != kIOReturnSuccess ||
        size != info.size())
        return false;

    return asciiField(info.data() + 0x00, 8) == "bridgeCo" &&
           le32(info.data() + 0x08) == 1 &&
           le32(info.data() + 0x0c) == 0 &&
           le32(info.data() + 0x18) == 0x83 &&
           le32(info.data() + 0x1c) == 1 &&
           asciiField(info.data() + 0x20, 8) == "20070713" &&
           le32(info.data() + 0x30) == 0 &&
           le32(info.data() + 0x38) == 0x20080000 &&
           le32(info.data() + 0x3c) == 0x00180000;
}

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
                 UInt32 length,
                 bool raw) {
    ctx.received = false;
    ctx.length = 0;
    ctx.bytes.fill(0);

    if (raw) {
        std::cout << "        command:  ";
        printBytes(command, length);
        std::cout << '\n';
    }

    UInt32 size = length;
    const IOReturn kr = device.write(kAddressHi, kFcpCommandLo,
                                     command, size);
    if (kr != kIOReturnSuccess) {
        std::cout << "        FCP write failed: 0x" << std::hex << kr
                  << std::dec << '\n';
        return false;
    }

    const CFAbsoluteTime deadline =
        CFAbsoluteTimeGetCurrent() + kFcpTimeoutSeconds;
    while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);

    if (!ctx.received) {
        std::cout << "        FCP response timeout\n";
        return false;
    }

    if (raw) {
        std::cout << "        response: ";
        printBytes(ctx.bytes.data(), ctx.length);
        std::cout << '\n';
    }
    return true;
}

bool validControlResponse(UInt8 response) {
    return response == 0x09 || response == 0x0c ||
           response == 0x0d || response == 0x0f;
}

bool setOutput48(macfw::FireWireDevice& device,
                 ResponseContext& ctx,
                 bool raw) {
    const UInt8 command[8] = {
        0x00, 0xff, 0x18, 0x00, 0x90, 0x02, 0xff, 0xff
    };
    if (!transaction(device, ctx, command,
                     static_cast<UInt32>(sizeof(command)), raw))
        return false;
    if (ctx.length < 8 || !validControlResponse(ctx.bytes[0]))
        return false;
    return ctx.bytes[1] == 0xff &&
           ctx.bytes[2] == 0x18 &&
           ctx.bytes[3] == 0x00 &&
           ctx.bytes[4] == 0x90 &&
           (ctx.bytes[5] & 0x07) == 0x02;
}

bool readInputRate(macfw::FireWireDevice& device,
                   ResponseContext& ctx,
                   unsigned& rate,
                   bool raw) {
    const UInt8 command[8] = {
        0x01, 0xff, 0x19, 0x00, 0x90, 0xff, 0xff, 0xff
    };
    if (!transaction(device, ctx, command,
                     static_cast<UInt32>(sizeof(command)), raw))
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

IOReturn getGeneration(IOFireWireLibDeviceRef native, UInt32& generation) {
    generation = 0;
    return (*native)->GetBusGeneration(native, &generation);
}

bool run(bool execute, bool raw) {
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
    const UInt32 initialGeneration = device.generation();

    std::cout << "matched operational unit:\n"
              << "    product: " << kProduct << '\n'
              << "    generation: " << initialGeneration << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID()
              << std::dec << '\n';

    const bool fingerprintOk = validateInfo(device);
    std::cout << "    BeBoB operational fingerprint: "
              << (fingerprintOk ? "PASS" : "FAIL") << '\n';
    if (!fingerprintOk) {
        device.close();
        return false;
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
    bool diagnosticComplete = false;
    std::uint32_t opcr0 = 0;
    std::uint32_t ipcr0 = 0;

    ResponseContext ctx;
    ctx.expectedNode = device.nodeID();
    IOFireWireLibPseudoAddressSpaceRef responseSpace = nullptr;

    if ((*native)->AddCallbackDispatcherToRunLoop(
            native, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
        std::cout << "callback dispatcher setup failed\n";
        device.close();
        return false;
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
        const bool rateOk = readInputRate(device, ctx, currentRate, raw) &&
                            currentRate == kRate;
        std::cout << "    authoritative INPUT rate: " << currentRate
                  << " Hz -> " << (rateOk ? "PASS" : "FAIL") << '\n';
        if (!rateOk) goto cleanup;
    }

    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) {
        std::cout << "PCR read failed\n";
        goto cleanup;
    }

    {
        const auto op = macfw::cmp::decodePcr(opcr0);
        const auto ip = macfw::cmp::decodePcr(ipcr0);
        std::cout << "OUTPUT-generation-watch preflight:\n"
                  << "    oPCR[0]: 0x" << std::hex << opcr0 << std::dec
                  << " online=" << (op.online ? "yes" : "no")
                  << " p2p=" << static_cast<unsigned>(op.p2pConnections) << '\n'
                  << "    iPCR[0]: 0x" << std::hex << ipcr0 << std::dec
                  << " online=" << (ip.online ? "yes" : "no")
                  << " p2p=" << static_cast<unsigned>(ip.p2pConnections) << '\n'
                  << "    playback: blocking silence, DBS=7, 8/8/8/NODATA\n"
                  << "    INPUT rate CONTROL: WILL NOT BE SENT\n";
        if (!macfw::cmp::ready(op) || !macfw::cmp::ready(ip)) {
            std::cout << "status: REFUSED - PCR0 offline or already connected\n";
            goto cleanup;
        }
    }

    if (!execute) {
        std::cout << "status: PASS - dry run only\n";
        diagnosticComplete = true;
        goto cleanup;
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

        auto receiveRing = macfw::AmdtpReceiveRing::create(
            device, kCapturePackets, kCaptureMaxPayload);
        auto transmitRing = macfw::fw1814::BlockingSilenceTransmitRing::create48k(
            device, firstTxCycle, kPlaybackDbs,
            kPlaybackPcmChannels, kTxPackets);
        auto capture = macfw::IsochAllocation::create(
            device, macfw::IsochAllocation::Direction::DeviceToHost,
            kCaptureMaxPayload);
        auto playback = macfw::IsochAllocation::create(
            device, macfw::IsochAllocation::Direction::HostToDevice,
            kPlaybackMaxPayload);
        IOFireWireLibIsochChannelRef captureChannel = nullptr;
        IOFireWireLibIsochChannelRef playbackChannel = nullptr;

        if (!receiveRing || !transmitRing || !capture || !playback) {
            std::cout << "ISO resource creation failed\n";
            goto cleanup_stream;
        }

        captureChannel = capture.nativeChannel();
        playbackChannel = playback.nativeChannel();

        {
            const IOReturn kr = (*captureChannel)->AddListener(
                captureChannel,
                reinterpret_cast<IOFireWireLibIsochPortRef>(
                    receiveRing.nativeLocalPort()));
            if (kr != kIOReturnSuccess) {
                std::cout << "capture AddListener failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }

        {
            const IOReturn kr = playback.bindHostToDeviceTalkerFirst(
                transmitRing.nativeLocalPort());
            if (kr != kIOReturnSuccess) {
                std::cout << "playback SetTalker failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }

        if ((*native)->AddIsochCallbackDispatcherToRunLoop(
                native, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
            std::cout << "isoch callback dispatcher setup failed\n";
            goto cleanup_stream;
        }
        isochDispatcher = true;
        if ((*native)->TurnOnNotification(native)) notifications = true;

        if (playback.allocate() != kIOReturnSuccess ||
            capture.allocate() != kIOReturnSuccess) {
            std::cout << "ISO allocation failed\n";
            goto cleanup_stream;
        }

        if (macfw::cmp::connectOpcr0(
                device, opcr0, capture.channel(), capture.speed()) !=
            kIOReturnSuccess) {
            std::cout << "connect oPCR[0] failed\n";
            goto cleanup_stream;
        }
        opcrConnected = true;

        if (macfw::cmp::connectIpcr0(
                device, ipcr0, playback.channel()) != kIOReturnSuccess) {
            std::cout << "connect iPCR[0] failed\n";
            goto cleanup_stream;
        }
        ipcrConnected = true;

        std::cout << "CMP: both PCRs connected\n";

        if ((*playbackChannel)->Start(playbackChannel) != kIOReturnSuccess) {
            std::cout << "playback start failed\n";
            goto cleanup_stream;
        }
        playbackStarted = true;

        if ((*captureChannel)->Start(captureChannel) != kIOReturnSuccess) {
            std::cout << "capture start failed\n";
            goto cleanup_stream;
        }
        captureStarted = true;

        std::cout << "blocking TX + RX DMA: started\n";
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);

        UInt32 beforeOutput = 0;
        if (getGeneration(native, beforeOutput) != kIOReturnSuccess) {
            std::cout << "generation read before OUTPUT failed\n";
            goto cleanup_stream;
        }
        std::cout << "generation before OUTPUT: " << beforeOutput << '\n';
        if (beforeOutput != initialGeneration) {
            std::cout << "BUS RESET DETECTED before OUTPUT; refusing FCP CONTROL\n";
            generationChanged = true;
            diagnosticComplete = true;
            goto cleanup_stream;
        }

        std::cout << "sending OUTPUT 48000 Hz CONTROL only...\n";
        {
            const bool outputOk = setOutput48(device, ctx, raw);
            std::cout << "    OUTPUT result: "
                      << (outputOk ? "PASS" : "FAIL") << '\n';
        }

        std::cout << "watching bus generation for 300 ms; INPUT CONTROL is skipped\n";
        for (unsigned i = 1; i <= 30; ++i) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
            UInt32 observed = 0;
            if (getGeneration(native, observed) != kIOReturnSuccess) {
                std::cout << "    generation read failed at +" << (i * 10)
                          << " ms\n";
                break;
            }
            if (observed != beforeOutput) {
                std::cout << "    BUS RESET DETECTED at <= +" << (i * 10)
                          << " ms: generation " << beforeOutput
                          << " -> " << observed << '\n';
                generationChanged = true;
                break;
            }
        }

        if (!generationChanged)
            std::cout << "    generation remained stable at "
                      << beforeOutput << '\n';

        diagnosticComplete = true;

cleanup_stream:
        if (captureStarted && captureChannel)
            (*captureChannel)->Stop(captureChannel);
        if (playbackStarted && playbackChannel)
            (*playbackChannel)->Stop(playbackChannel);

        if (generationChanged) {
            std::cout << "bus generation changed: skipping PCR restore writes from stale generation\n";
        } else {
            if (ipcrConnected) {
                const IOReturn kr = macfw::cmp::restore(
                    device, macfw::cmp::kIpcr0AddressLo, ipcr0);
                std::cout << "restore iPCR[0]: "
                          << (kr == kIOReturnSuccess ? "success" : "failed")
                          << '\n';
            }
            if (opcrConnected) {
                const IOReturn kr = macfw::cmp::restore(
                    device, macfw::cmp::kOpcr0AddressLo, opcr0);
                std::cout << "restore oPCR[0]: "
                          << (kr == kIOReturnSuccess ? "success" : "failed")
                          << '\n';
            }
        }
    }

cleanup:
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

    device.close();

    if (diagnosticComplete) {
        std::cout << "status: PASS - OUTPUT-only generation diagnostic completed\n";
        return true;
    }
    std::cout << "status: FAIL - diagnostic did not complete\n";
    return false;
}

void usage(const char* argv0) {
    std::cout << "usage: " << argv0 << " [--execute] [--raw]\n";
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
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 64;
        }
    }

    std::cout << "macfw fw1814capture48-output-watch — OUTPUT-only bus generation diagnostic\n\n";
    return run(execute, raw) ? 0 : 1;
}
