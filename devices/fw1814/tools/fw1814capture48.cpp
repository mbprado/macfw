#include "macfw/am824.h"
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
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kProduct = "FW 1814";
constexpr UInt16 kAddressHi = 0xffff;
constexpr UInt32 kInfoLo = 0xc8020000;
constexpr UInt32 kFcpCommandLo = 0xf0000b00;
constexpr UInt32 kFcpResponseLo = 0xf0000d00;
constexpr UInt32 kFcpResponseSize = 0x200;
constexpr double kFcpTimeoutSeconds = 1.0;

constexpr unsigned kRate = 48000;
constexpr unsigned kExpectedDbs = 11;   // 10 PCM + 1 MIDI in S/PDIF mode.
constexpr unsigned kExpectedFdf = 0x02; // IEC 61883-6 / AM824 48 kHz SFC.
constexpr UInt32 kMaxPayloadBytes = 272; // 8-byte CIP + 6 * 11 * 4.
constexpr std::size_t kPacketCount = 64;

struct ResponseContext {
    UInt16 expectedNode = 0;
    bool received = false;
    UInt32 length = 0;
    std::array<UInt8, kFcpResponseSize> bytes{};
};

struct PositionStats {
    std::map<unsigned, std::uint64_t> labels;
    std::uint64_t mblaWords = 0;
    std::int32_t minimum = std::numeric_limits<std::int32_t>::max();
    std::int32_t maximum = std::numeric_limits<std::int32_t>::min();
    std::uint32_t peak = 0;
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

bool fcpTransaction(macfw::FireWireDevice& device,
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

    if (raw && ctx.received) {
        std::cout << "        response: ";
        printBytes(ctx.bytes.data(), ctx.length);
        std::cout << '\n';
    }
    return ctx.received;
}

bool validControlResponse(UInt8 response) {
    return response == 0x09 || response == 0x0c ||
           response == 0x0d || response == 0x0f;
}

bool setSignalRate(macfw::FireWireDevice& device,
                   ResponseContext& ctx,
                   UInt8 opcode,
                   bool raw) {
    // Standard AV/C PLUG SIGNAL FORMAT CONTROL. 48 kHz uses SFC 0x02.
    const UInt8 command[8] = {
        0x00, 0xff, opcode, 0x00, 0x90, 0x02, 0xff, 0xff
    };
    if (!fcpTransaction(device, ctx, command,
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
    if (!fcpTransaction(device, ctx, command,
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
    const UInt8 sfc = ctx.bytes[5] & 0x07;
    rate = rates[sfc];
    return rate != 0;
}

bool specialStreamKick(macfw::FireWireDevice& device,
                       ResponseContext& ctx,
                       bool raw) {
    // snd-bebob special-firmware start sequence: after the CMP/AMDTP stream
    // is established, reasserting the current rate starts transmission.
    // FW1814 additionally requires 100 ms between OUTPUT and INPUT controls.
    std::cout << "FW1814 special stream kick:\n";
    std::cout << "    reassert OUTPUT 48000 Hz...\n";
    const bool outputOk = setSignalRate(device, ctx, 0x18, raw);
    std::cout << "        result: " << (outputOk ? "PASS" : "FAIL") << '\n';
    if (!outputOk)
        return false;

    std::cout << "    waiting 100 ms before INPUT rate CONTROL...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "    reassert INPUT 48000 Hz...\n";
    const bool inputOk = setSignalRate(device, ctx, 0x19, raw);
    std::cout << "        result: " << (inputOk ? "PASS" : "FAIL") << '\n';
    if (!inputOk)
        return false;

    unsigned readback = 0;
    const bool readbackOk = readInputRate(device, ctx, readback, raw) &&
                            readback == kRate;
    std::cout << "    INPUT rate readback: " << readback << " Hz -> "
              << (readbackOk ? "PASS" : "FAIL") << '\n';
    return readbackOk;
}

void accumulatePositions(const macfw::amdtp::PacketView& packet,
                         std::vector<PositionStats>& stats,
                         std::uint64_t& events,
                         std::uint64_t& malformed) {
    if (!packet.hasCip() || packet.isNoData())
        return;

    const auto cip = packet.cip();
    if (cip.dbs != kExpectedDbs ||
        cip.fmt != 0x10 ||
        cip.fdf != kExpectedFdf) {
        ++malformed;
        return;
    }

    const std::size_t eventBytes = kExpectedDbs * 4;
    if (packet.dataLength() % eventBytes != 0) {
        ++malformed;
        return;
    }

    const auto* p = packet.data();
    const std::size_t eventCount = packet.dataLength() / eventBytes;
    for (std::size_t event = 0; event < eventCount; ++event) {
        for (unsigned pos = 0; pos < kExpectedDbs; ++pos) {
            const std::uint32_t word = macfw::am824::be32(p + pos * 4);
            const unsigned label = static_cast<unsigned>((word >> 24) & 0xff);
            auto& s = stats[pos];
            ++s.labels[label];

            if (label == macfw::am824::kMbla24Label) {
                const std::int32_t sample = macfw::am824::signExtend24(word);
                ++s.mblaWords;
                s.minimum = std::min(s.minimum, sample);
                s.maximum = std::max(s.maximum, sample);
                const std::int64_t wide = sample;
                const std::uint32_t magnitude =
                    static_cast<std::uint32_t>(wide < 0 ? -wide : wide);
                s.peak = std::max(s.peak, magnitude);
            }
        }
        ++events;
        p += eventBytes;
    }
}

void printFormation(const macfw::AmdtpReceiveRing& ring, bool raw,
                    bool& formationOk) {
    std::vector<PositionStats> positions(kExpectedDbs);
    std::uint64_t events = 0;
    std::uint64_t malformed = 0;
    std::size_t shown = 0;

    for (std::size_t i = 0; i < ring.packetCount(); ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched())
            continue;

        const auto packet = slot.packet();
        if (shown < 12) {
            std::cout << "    packet " << i
                      << ": len=" << slot.packetLength();
            if (packet.hasCip()) {
                const auto cip = packet.cip();
                std::cout << " CIP{dbs=" << static_cast<unsigned>(cip.dbs)
                          << " dbc=" << static_cast<unsigned>(cip.dbc)
                          << " fmt=0x" << std::hex
                          << static_cast<unsigned>(cip.fmt)
                          << " fdf=0x" << static_cast<unsigned>(cip.fdf)
                          << " syt=0x" << cip.syt << std::dec << "}";
            }
            std::cout << '\n';

            if (raw && slot.packetLength() &&
                slot.packetLength() <= slot.capacity) {
                const std::size_t n =
                    std::min<std::size_t>(slot.packetLength(), 96);
                std::cout << "        raw: ";
                printBytes(slot.payload, n);
                if (n < slot.packetLength())
                    std::cout << " ...";
                std::cout << '\n';
            }
            ++shown;
        }

        accumulatePositions(packet, positions, events, malformed);
    }

    std::cout << "AM824 formation summary:\n"
              << "    decoded data-block events: " << events << '\n'
              << "    malformed packets:         " << malformed << '\n';

    unsigned mblaPositions = 0;
    unsigned nonMblaPositions = 0;
    for (unsigned pos = 0; pos < kExpectedDbs; ++pos) {
        const auto& s = positions[pos];
        std::cout << "    pos " << std::setw(2) << pos << ": labels";
        if (s.labels.empty()) {
            std::cout << " <none>";
        } else {
            for (const auto& entry : s.labels) {
                std::cout << " 0x" << std::hex << std::setw(2)
                          << std::setfill('0') << entry.first
                          << std::dec << std::setfill(' ')
                          << "(" << entry.second << ")";
            }
        }
        if (s.mblaWords) {
            ++mblaPositions;
            std::cout << "  MBLA=" << s.mblaWords
                      << " min=" << s.minimum
                      << " max=" << s.maximum
                      << " peak=" << s.peak;
        } else if (!s.labels.empty()) {
            ++nonMblaPositions;
        }
        std::cout << '\n';
    }

    std::cout << "    MBLA/PCM positions observed: " << mblaPositions << '\n'
              << "    non-MBLA positions observed: " << nonMblaPositions << '\n';

    formationOk = ring.touchedCount() > 0 &&
                  events > 0 &&
                  malformed == 0 &&
                  mblaPositions == 10 &&
                  nonMblaPositions == 1;
    std::cout << "formation validation: "
              << (formationOk ? "PASS" : "INCOMPLETE") << '\n';
}

bool run(bool execute, bool raw) {
    auto device = macfw::FireWireDevice::findByProductName(kProduct);
    if (!device) {
        std::cout << "No operational FW 1814 unit found.\n";
        return false;
    }

    std::cout << "matched operational unit:\n"
              << "    product: " << kProduct << '\n'
              << "    generation: " << device.generation() << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID()
              << std::dec << '\n';

    if (device.open() != kIOReturnSuccess) {
        std::cout << "open failed\n";
        return false;
    }

    const bool fingerprintOk = validateInfo(device);
    std::cout << "    BeBoB operational fingerprint: "
              << (fingerprintOk ? "PASS" : "FAIL") << '\n';
    if (!fingerprintOk)
        return false;

    auto native = device.nativeHandle();
    bool callbackDispatcher = false;
    bool isochDispatcher = false;
    bool notifications = false;
    bool responseNotification = false;
    bool captureStarted = false;
    bool opcrConnected = false;
    bool success = false;
    bool formationOk = false;
    std::uint32_t opcr0 = 0;

    ResponseContext ctx;
    ctx.expectedNode = device.nodeID();
    IOFireWireLibPseudoAddressSpaceRef responseSpace = nullptr;

    if ((*native)->AddCallbackDispatcherToRunLoop(
            native, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
        std::cout << "callback dispatcher setup failed\n";
        return false;
    }
    callbackDispatcher = true;

    responseSpace = (*native)->CreateInitialUnitsPseudoAddressSpace(
        native, kFcpResponseLo, kFcpResponseSize, &ctx, 1024, nullptr,
        kFWAddressSpaceNoReadAccess | kFWAddressSpaceShareIfExists,
        CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
    if (!responseSpace) {
        std::cout << "FCP response address-space setup failed\n";
        goto cleanup_preiso;
    }
    (*responseSpace)->SetWriteHandler(responseSpace, responseHandler);
    if (!(*responseSpace)->TurnOnNotification(responseSpace)) {
        std::cout << "FCP response notification setup failed\n";
        goto cleanup_preiso;
    }
    responseNotification = true;

    {
        unsigned currentRate = 0;
        const bool rateOk = readInputRate(device, ctx, currentRate, raw) &&
                            currentRate == kRate;
        std::cout << "    authoritative INPUT rate: " << currentRate
                  << " Hz -> " << (rateOk ? "PASS" : "FAIL") << '\n';
        if (!rateOk)
            goto cleanup_preiso;
    }

    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess) {
        std::cout << "oPCR[0] read failed\n";
        goto cleanup_preiso;
    }

    {
        const auto pcr = macfw::cmp::decodePcr(opcr0);
        std::cout << "receive-only preflight:\n"
                  << "    oPCR[0]:       0x" << std::hex << opcr0
                  << std::dec << '\n'
                  << "    online:        " << (pcr.online ? "yes" : "no") << '\n'
                  << "    p2p:           "
                  << static_cast<unsigned>(pcr.p2pConnections) << '\n'
                  << "    expected DBS:  " << kExpectedDbs << '\n'
                  << "    max payload:   " << kMaxPayloadBytes << " bytes\n"
                  << "    packet slots:  " << kPacketCount << '\n';
        if (!macfw::cmp::ready(pcr)) {
            std::cout << "status: REFUSED - device OUTPUT plug is offline or already connected\n";
            goto cleanup_preiso;
        }
    }

    if (!execute) {
        std::cout << "status: PASS - dry run only; no CMP connection, ISO stream, or rate CONTROL sent\n";
        success = true;
        goto cleanup_preiso;
    }

    {
        auto ring = macfw::AmdtpReceiveRing::create(
            device, kPacketCount, kMaxPayloadBytes);
        auto capture = macfw::IsochAllocation::create(
            device,
            macfw::IsochAllocation::Direction::DeviceToHost,
            kMaxPayloadBytes);
        if (!ring || !capture) {
            std::cout << "ISO receive resource creation failed\n";
            goto cleanup_preiso;
        }

        auto channel = capture.nativeChannel();
        const IOReturn listenerKr = (*channel)->AddListener(
            channel,
            reinterpret_cast<IOFireWireLibIsochPortRef>(ring.nativeLocalPort()));
        if (listenerKr != kIOReturnSuccess) {
            std::cout << "AddListener failed: 0x" << std::hex << listenerKr
                      << std::dec << '\n';
            goto cleanup_stream;
        }

        if ((*native)->AddIsochCallbackDispatcherToRunLoop(
                native, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
            std::cout << "isoch callback dispatcher setup failed\n";
            goto cleanup_stream;
        }
        isochDispatcher = true;

        if ((*native)->TurnOnNotification(native))
            notifications = true;

        {
            const IOReturn kr = capture.allocate();
            if (kr != kIOReturnSuccess) {
                std::cout << "capture ISO resource allocation failed: 0x"
                          << std::hex << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }

        std::cout << "capture ISO resource: channel=" << capture.channel()
                  << " speed=" << static_cast<unsigned>(capture.speed()) << '\n';

        {
            const IOReturn kr = macfw::cmp::connectOpcr0(
                device, opcr0, capture.channel(), capture.speed());
            if (kr != kIOReturnSuccess) {
                std::cout << "connect oPCR[0] failed: 0x" << std::hex << kr
                          << std::dec << '\n';
                goto cleanup_stream;
            }
        }
        opcrConnected = true;
        std::cout << "CMP: device OUTPUT connected to host receive channel\n";

        {
            const IOReturn kr = (*channel)->Start(channel);
            if (kr != kIOReturnSuccess) {
                std::cout << "capture channel start failed: 0x" << std::hex << kr
                          << std::dec << '\n';
                goto cleanup_stream;
            }
        }
        captureStarted = true;
        std::cout << "host receive DMA: started\n";

        if (!specialStreamKick(device, ctx, raw)) {
            std::cout << "status: FAIL - FW1814 special stream kick failed\n";
            goto cleanup_stream;
        }

        std::cout << "capture: waiting up to 2 s for " << kPacketCount
                  << " packet slots\n";
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false);

        std::cout << "capture: "
                  << (ring.completed() ? "NuDCL burst completed"
                                       : "timeout before burst completion")
                  << '\n'
                  << "    touched slots: " << ring.touchedCount()
                  << " / " << ring.packetCount() << '\n';

        printFormation(ring, raw, formationOk);
        success = formationOk;

cleanup_stream:
        if (captureStarted) {
            (*channel)->Stop(channel);
            captureStarted = false;
        }
        if (opcrConnected) {
            const IOReturn restore = macfw::cmp::restore(
                device, macfw::cmp::kOpcr0AddressLo, opcr0);
            std::cout << "restore oPCR[0]: "
                      << (restore == kIOReturnSuccess ? "success" : "failed")
                      << '\n';
            opcrConnected = false;
        }
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
        std::uint32_t after = 0;
        if (macfw::cmp::readOpcr0(device, after) == kIOReturnSuccess) {
            const bool restored = after == opcr0;
            std::cout << "post-test oPCR restore: "
                      << (restored ? "PASS" : "FAIL") << '\n';
            success = success && restored;
        } else {
            std::cout << "post-test oPCR restore: unable to read\n";
            success = false;
        }
    }

cleanup_preiso:
    if (responseNotification && responseSpace)
        (*responseSpace)->TurnOffNotification(responseSpace);
    if (responseSpace)
        (*responseSpace)->Release(responseSpace);
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
        if (arg == "--execute")
            execute = true;
        else if (arg == "--raw")
            raw = true;
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 64;
        }
    }

    std::cout << "macfw fw1814capture48 — guarded receive-only 48 kHz bring-up probe\n\n";
    return run(execute, raw) ? 0 : 1;
}
