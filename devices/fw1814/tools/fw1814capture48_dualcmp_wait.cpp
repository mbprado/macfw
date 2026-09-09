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
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr const char* kProduct = "FW 1814";
constexpr UInt16 kAddressHi = 0xffff;
constexpr UInt32 kInfoLo = 0xc8020000;
constexpr UInt32 kFcpCommandLo = 0xf0000b00;
constexpr UInt32 kFcpResponseLo = 0xf0000d00;
constexpr UInt32 kFcpResponseSize = 0x200;
constexpr double kFcpTimeoutSeconds = 1.0;

constexpr unsigned kRate = 48000;
constexpr UInt32 kCaptureMaxPayload = 272;  // 10 PCM + 1 MIDI, six events.
constexpr UInt32 kPlaybackMaxPayload = 176; // 6 PCM + 1 MIDI, six events.
constexpr std::size_t kPacketCount = 64;

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
    const IOReturn kr = device.write(kAddressHi, kFcpCommandLo, command, size);
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

bool setSignalRate(macfw::FireWireDevice& device,
                   ResponseContext& ctx,
                   UInt8 opcode,
                   bool raw) {
    const UInt8 command[8] = {
        0x00, 0xff, opcode, 0x00, 0x90, 0x02, 0xff, 0xff
    };
    if (!transaction(device, ctx, command,
                     static_cast<UInt32>(sizeof(command)), raw))
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

bool specialStreamKickNoReadback(macfw::FireWireDevice& device,
                                 ResponseContext& ctx,
                                 bool raw) {
    std::cout << "FW1814 special stream kick after BOTH CMP connections:\n";
    std::cout << "    reassert OUTPUT 48000 Hz...\n";
    const bool outputOk = setSignalRate(device, ctx, 0x18, raw);
    std::cout << "        result: " << (outputOk ? "PASS" : "FAIL") << '\n';
    if (!outputOk) return false;

    std::cout << "    waiting 100 ms before INPUT rate CONTROL...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "    reassert INPUT 48000 Hz...\n";
    const bool inputOk = setSignalRate(device, ctx, 0x19, raw);
    std::cout << "        result: " << (inputOk ? "PASS" : "FAIL") << '\n';
    if (!inputOk) return false;

    std::cout << "    immediate STATUS readback: intentionally skipped\n";
    return true;
}

void dumpReceive(const macfw::AmdtpReceiveRing& ring, bool raw) {
    std::cout << "capture result:\n"
              << "    touched slots: " << ring.touchedCount()
              << " / " << ring.packetCount() << '\n';

    std::size_t shown = 0;
    for (std::size_t i = 0; i < ring.packetCount() && shown < 12; ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched()) continue;

        std::cout << "    packet " << i << ": len=" << slot.packetLength();
        const auto packet = slot.packet();
        if (packet.hasCip()) {
            const auto cip = packet.cip();
            std::cout << " CIP{sid=" << static_cast<unsigned>(cip.sid)
                      << " dbs=" << static_cast<unsigned>(cip.dbs)
                      << " dbc=" << static_cast<unsigned>(cip.dbc)
                      << " fmt=0x" << std::hex << static_cast<unsigned>(cip.fmt)
                      << " fdf=0x" << static_cast<unsigned>(cip.fdf)
                      << " syt=0x" << cip.syt << std::dec << "}";
        }
        std::cout << '\n';

        if (raw && slot.packetLength() && slot.packetLength() <= slot.capacity) {
            const std::size_t n = std::min<std::size_t>(slot.packetLength(), 64);
            std::cout << "        raw: ";
            printBytes(slot.payload, n);
            if (n < slot.packetLength()) std::cout << " ...";
            std::cout << '\n';
        }
        ++shown;
    }
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

    std::cout << "matched operational unit:\n"
              << "    product: " << kProduct << '\n'
              << "    generation: " << device.generation() << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID()
              << std::dec << '\n';

    const bool fingerprintOk = validateInfo(device);
    std::cout << "    BeBoB operational fingerprint: "
              << (fingerprintOk ? "PASS" : "FAIL") << '\n';
    if (!fingerprintOk) {
        device.close();
        return false;
    }

    auto native = device.nativeHandle();
    bool callbackDispatcher = false;
    bool isochDispatcher = false;
    bool notifications = false;
    bool responseNotification = false;
    bool captureStarted = false;
    bool opcrConnected = false;
    bool ipcrConnected = false;
    bool success = false;
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
        std::cout << "dual-CMP/no-readback preflight:\n"
                  << "    oPCR[0]: 0x" << std::hex << opcr0 << std::dec
                  << " online=" << (op.online ? "yes" : "no")
                  << " p2p=" << static_cast<unsigned>(op.p2pConnections) << '\n'
                  << "    iPCR[0]: 0x" << std::hex << ipcr0 << std::dec
                  << " online=" << (ip.online ? "yes" : "no")
                  << " p2p=" << static_cast<unsigned>(ip.p2pConnections) << '\n'
                  << "    capture max payload:  " << kCaptureMaxPayload << " bytes\n"
                  << "    companion reservation: " << kPlaybackMaxPayload << " bytes\n";
        if (!macfw::cmp::ready(op) || !macfw::cmp::ready(ip)) {
            std::cout << "status: REFUSED - PCR0 offline or already connected\n";
            goto cleanup;
        }
    }

    if (!execute) {
        std::cout << "status: PASS - dry run only; both PCRs are available\n";
        success = true;
        goto cleanup;
    }

    {
        auto ring = macfw::AmdtpReceiveRing::create(
            device, kPacketCount, kCaptureMaxPayload);
        auto capture = macfw::IsochAllocation::create(
            device, macfw::IsochAllocation::Direction::DeviceToHost,
            kCaptureMaxPayload);
        auto companion = macfw::IsochAllocation::create(
            device, macfw::IsochAllocation::Direction::HostToDevice,
            kPlaybackMaxPayload);
        IOFireWireLibIsochChannelRef captureChannel = nullptr;

        if (!ring || !capture || !companion) {
            std::cout << "ISO resource creation failed\n";
            goto cleanup_stream;
        }

        captureChannel = capture.nativeChannel();
        {
            const IOReturn kr = (*captureChannel)->AddListener(
                captureChannel,
                reinterpret_cast<IOFireWireLibIsochPortRef>(ring.nativeLocalPort()));
            if (kr != kIOReturnSuccess) {
                std::cout << "capture AddListener failed: 0x" << std::hex
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

        {
            const IOReturn kr = capture.allocate();
            if (kr != kIOReturnSuccess) {
                std::cout << "capture allocation failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }
        {
            const IOReturn kr = companion.allocate();
            if (kr != kIOReturnSuccess) {
                std::cout << "companion allocation failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }

        std::cout << "ISO resources:\n"
                  << "    capture:   channel=" << capture.channel()
                  << " speed=" << static_cast<unsigned>(capture.speed()) << '\n'
                  << "    companion: channel=" << companion.channel()
                  << " speed=" << static_cast<unsigned>(companion.speed()) << '\n';

        {
            const IOReturn kr = macfw::cmp::connectOpcr0(
                device, opcr0, capture.channel(), capture.speed());
            if (kr != kIOReturnSuccess) {
                std::cout << "connect oPCR[0] failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }
        opcrConnected = true;

        {
            const IOReturn kr = macfw::cmp::connectIpcr0(
                device, ipcr0, companion.channel());
            if (kr != kIOReturnSuccess) {
                std::cout << "connect iPCR[0] failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }
        ipcrConnected = true;

        std::cout << "CMP: BOTH oPCR[0] and iPCR[0] connected\n"
                  << "companion TX DMA: intentionally NOT started\n";

        {
            const IOReturn kr = (*captureChannel)->Start(captureChannel);
            if (kr != kIOReturnSuccess) {
                std::cout << "capture channel start failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }
        captureStarted = true;
        std::cout << "host receive DMA: started\n";

        if (!specialStreamKickNoReadback(device, ctx, raw)) {
            std::cout << "status: FAIL - special stream kick CONTROL failed\n";
            goto cleanup_stream;
        }

        std::cout << "capture: waiting up to 2 s for packets\n";
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false);
        dumpReceive(ring, raw);

        success = ring.touchedCount() > 0;
        std::cout << "dual-CMP/no-readback experiment: "
                  << (success ? "PACKETS RECEIVED" : "NO PACKETS") << '\n';

cleanup_stream:
        if (captureStarted && captureChannel) {
            (*captureChannel)->Stop(captureChannel);
            captureStarted = false;
        }
        if (ipcrConnected) {
            const IOReturn kr = macfw::cmp::restore(
                device, macfw::cmp::kIpcr0AddressLo, ipcr0);
            std::cout << "restore iPCR[0]: "
                      << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
            ipcrConnected = false;
        }
        if (opcrConnected) {
            const IOReturn kr = macfw::cmp::restore(
                device, macfw::cmp::kOpcr0AddressLo, opcr0);
            std::cout << "restore oPCR[0]: "
                      << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
            opcrConnected = false;
        }
        companion.release();
        capture.release();
    }

    if (notifications) {
        (*native)->TurnOffNotification(native);
        notifications = false;
    }
    if (isochDispatcher) {
        (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
        isochDispatcher = false;
    }

    if (execute) {
        std::uint32_t opAfter = 0;
        std::uint32_t ipAfter = 0;
        const bool readable =
            macfw::cmp::readOpcr0(device, opAfter) == kIOReturnSuccess &&
            macfw::cmp::readIpcr0(device, ipAfter) == kIOReturnSuccess;
        const bool restored = readable && opAfter == opcr0 && ipAfter == ipcr0;
        std::cout << "post-test PCR restore: "
                  << (restored ? "PASS" : "FAIL") << '\n';
        success = success && restored;

        unsigned postRate = 0;
        const bool postRateOk = readInputRate(device, ctx, postRate, raw);
        std::cout << "post-disconnect INPUT rate readback: ";
        if (postRateOk)
            std::cout << postRate << " Hz"
                      << (postRate == kRate ? " -> PASS" : " -> CHANGED");
        else
            std::cout << "unavailable (non-fatal diagnostic)";
        std::cout << '\n';
    }

cleanup:
    if (responseNotification && responseSpace)
        (*responseSpace)->TurnOffNotification(responseSpace);
    if (responseSpace) (*responseSpace)->Release(responseSpace);
    if (callbackDispatcher)
        (*native)->RemoveCallbackDispatcherFromRunLoop(native);
    device.close();
    return success;
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

    std::cout << "macfw fw1814capture48-dualcmp-wait — no-readback startup diagnostic\n\n";
    return run(execute, raw) ? 0 : 1;
}
