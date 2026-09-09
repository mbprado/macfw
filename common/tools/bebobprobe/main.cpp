#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

constexpr UInt16 kAddressHi = 0xffff;
constexpr UInt32 kFcpCommandLo = 0xf0000b00;
constexpr UInt32 kFcpResponseLo = 0xf0000d00;
constexpr UInt32 kFcpResponseSize = 0x200;
constexpr double kTimeoutSeconds = 1.0;
constexpr unsigned kMaxFormatEntries = 16;

constexpr UInt32 kOmprLo = 0xf0000900;
constexpr UInt32 kOpcr0Lo = 0xf0000904;
constexpr UInt32 kImprLo = 0xf0000980;
constexpr UInt32 kIpcr0Lo = 0xf0000984;

struct ResponseContext {
    UInt16 expectedNode = 0;
    bool received = false;
    UInt32 length = 0;
    std::array<UInt8, kFcpResponseSize> bytes{};
};

struct Formation {
    unsigned rate = 0;
    unsigned pcmChannels = 0;
    unsigned midiChannels = 0;
    unsigned otherChannels = 0;
    unsigned clusters = 0;
};

bool gRaw = false;
bool gBridgeCoFormats = false;
bool gMaudioSpecial = false;

std::string stringProperty(io_registry_entry_t service, const char* key) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                               kCFStringEncodingUTF8);
    if (!k) return {};
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, k,
                                                       kCFAllocatorDefault, 0);
    CFRelease(k);
    if (!value) return {};

    std::string result;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        char buffer[1024] = {};
        if (CFStringGetCString(static_cast<CFStringRef>(value), buffer,
                               sizeof(buffer), kCFStringEncodingUTF8))
            result = buffer;
    }
    CFRelease(value);
    return result;
}

bool numberProperty(io_registry_entry_t service, const char* key,
                    std::uint64_t& out) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                               kCFStringEncodingUTF8);
    if (!k) return false;
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, k,
                                                       kCFAllocatorDefault, 0);
    CFRelease(k);
    if (!value) return false;

    bool ok = false;
    if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        long long n = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(value),
                             kCFNumberLongLongType, &n)) {
            out = static_cast<std::uint64_t>(n);
            ok = true;
        }
    }
    CFRelease(value);
    return ok;
}

void printBytes(const UInt8* p, UInt32 n) {
    for (UInt32 i = 0; i < n; ++i) {
        if (i) std::cout << ' ';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(p[i]);
    }
    std::cout << std::dec << std::setfill(' ');
}

unsigned rateForSfc(UInt8 sfc) {
    static constexpr unsigned table[] = {
        32000, 44100, 48000, 88200, 96000, 176400, 192000, 0
    };
    return sfc < 8 ? table[sfc] : 0;
}

unsigned rateForBridgeCoCode(UInt8 code) {
    switch (code) {
        case 0x02: return 32000;
        case 0x03: return 44100;
        case 0x04: return 48000;
        case 0x0a: return 88200;
        case 0x05: return 96000;
        case 0x06: return 176400;
        case 0x07: return 192000;
        default: return 0;
    }
}

IOReturn readAbsolute(IOFireWireLibDeviceRef device, UInt32 generation,
                      UInt16 node, UInt32 lo, void* buffer, UInt32& size) {
    FWAddress address{};
    address.nodeID = node;
    address.addressHi = kAddressHi;
    address.addressLo = lo;
    return (*device)->Read(device, 0, &address, buffer, &size, true, generation);
}

IOReturn writeFcp(IOFireWireLibDeviceRef device, UInt32 generation,
                  UInt16 node, const void* buffer, UInt32& size) {
    FWAddress address{};
    address.nodeID = node;
    address.addressHi = kAddressHi;
    address.addressLo = kFcpCommandLo;
    return (*device)->Write(device, 0, &address, buffer, &size, true, generation);
}

UInt32 responseHandler(IOFireWireLibPseudoAddressSpaceRef space,
                       FWClientCommandID commandID, UInt32 packetLen,
                       void* packet, UInt16 srcNodeID, UInt32, UInt32,
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

bool transaction(IOFireWireLibDeviceRef device, UInt32 generation,
                 UInt16 node, ResponseContext& ctx,
                 const UInt8* command, UInt32 length) {
    ctx.received = false;
    ctx.length = 0;
    ctx.bytes.fill(0);

    if (gRaw) {
        std::cout << "        command:  ";
        printBytes(command, length);
        std::cout << '\n';
    }

    UInt32 size = length;
    const IOReturn kr = writeFcp(device, generation, node, command, size);
    if (kr != kIOReturnSuccess) {
        std::cout << "        FCP command write failed: 0x" << std::hex << kr
                  << std::dec << '\n';
        return false;
    }

    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
    while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);

    if (gRaw && ctx.received) {
        std::cout << "        response: ";
        printBytes(ctx.bytes.data(), ctx.length);
        std::cout << '\n';
    }
    return ctx.received;
}

bool readSignalRate(IOFireWireLibDeviceRef device, UInt32 generation,
                    UInt16 node, ResponseContext& ctx, UInt8 opcode,
                    unsigned& rate) {
    // Standard AV/C STATUS for OUTPUT/INPUT PLUG SIGNAL FORMAT, unit plug 0.
    const UInt8 command[8] = {
        0x01, 0xff, opcode, 0x00, 0x90, 0xff, 0xff, 0xff
    };
    if (!transaction(device, generation, node, ctx, command,
                     static_cast<UInt32>(sizeof(command))))
        return false;
    if (ctx.length < 8 ||
        (ctx.bytes[0] != 0x0c && ctx.bytes[0] != 0x0d) ||
        ctx.bytes[1] != 0xff || ctx.bytes[2] != opcode ||
        ctx.bytes[3] != 0x00 || ctx.bytes[4] != 0x90)
        return false;
    rate = rateForSfc(ctx.bytes[5] & 0x07);
    return rate != 0;
}

std::array<UInt8, 12> streamFormatCommand(UInt8 direction, UInt8 eid) {
    // BridgeCo extension: STREAM FORMAT SUPPORT / list request.
    // IMPORTANT: this is intentionally opt-in. M-Audio FW1814/ProjectMix
    // special firmware does not implement BridgeCo extensions.
    return {0x01, 0xff, 0x2f, 0xc1,
            direction, 0x00, 0x00, 0x00, 0xff,
            0x00, eid, 0x00};
}

bool decodeFormation(const UInt8* payload, UInt32 length,
                     Formation& formation) {
    if (length < 5 || payload[0] != 0x90 || payload[1] != 0x40)
        return false;

    formation.rate = rateForBridgeCoCode(payload[2]);
    formation.clusters = payload[4];
    const UInt32 required = 5 + formation.clusters * 2;
    if (length < required)
        return false;

    for (unsigned i = 0; i < formation.clusters; ++i) {
        const UInt8 channels = payload[5 + i * 2];
        const UInt8 format = payload[6 + i * 2];
        if (format == 0x06)
            formation.pcmChannels += channels;
        else if (format == 0x0d)
            formation.midiChannels += channels;
        else
            formation.otherChannels += channels;

        if (gRaw) {
            std::cout << "            cluster " << i
                      << ": channels=" << static_cast<unsigned>(channels)
                      << " format=0x" << std::hex
                      << static_cast<unsigned>(format) << std::dec << '\n';
        }
    }
    return formation.rate != 0;
}

void enumerateFormats(IOFireWireLibDeviceRef device, UInt32 generation,
                      UInt16 node, ResponseContext& ctx,
                      UInt8 direction, const char* label) {
    std::cout << "    " << label << " supported stream formats:\n";
    for (unsigned eid = 0; eid < kMaxFormatEntries; ++eid) {
        const auto command = streamFormatCommand(direction,
                                                  static_cast<UInt8>(eid));
        if (!transaction(device, generation, node, ctx,
                         command.data(), static_cast<UInt32>(command.size()))) {
            std::cout << "        entry " << eid << ": timeout\n";
            break;
        }
        if (!ctx.length || ctx.bytes[0] != 0x0c)
            break;
        if (ctx.length < 12 || ctx.bytes[10] != eid) {
            std::cout << "        entry " << eid
                      << ": unexpected response shape\n";
            break;
        }

        Formation formation;
        const UInt8* payload = ctx.bytes.data() + 11;
        const UInt32 payloadLength = ctx.length - 11;
        if (!decodeFormation(payload, payloadLength, formation)) {
            std::cout << "        entry " << eid << ": undecoded";
            if (gRaw) {
                std::cout << " payload=";
                printBytes(payload, payloadLength);
            }
            std::cout << '\n';
            continue;
        }

        std::cout << "        entry " << eid << ": " << formation.rate
                  << " Hz, PCM=" << formation.pcmChannels
                  << ", MIDI=" << formation.midiChannels;
        if (formation.otherChannels)
            std::cout << ", other=" << formation.otherChannels;
        std::cout << ", clusters=" << formation.clusters << '\n';
    }
}

bool readQuadlet(IOFireWireLibDeviceRef device, UInt32 generation,
                 UInt16 node, UInt32 lo, std::uint32_t& value) {
    std::array<UInt8, 4> bytes{};
    UInt32 size = static_cast<UInt32>(bytes.size());
    const IOReturn kr = readAbsolute(device, generation, node, lo,
                                     bytes.data(), size);
    if (kr != kIOReturnSuccess || size != bytes.size()) {
        std::cout << "        CMP read 0xffff" << std::hex << lo
                  << " failed: 0x" << kr << std::dec
                  << " (" << size << " bytes)\n";
        return false;
    }

    value = (static_cast<std::uint32_t>(bytes[0]) << 24) |
            (static_cast<std::uint32_t>(bytes[1]) << 16) |
            (static_cast<std::uint32_t>(bytes[2]) << 8) |
            static_cast<std::uint32_t>(bytes[3]);
    return true;
}

void printMpr(const char* name, std::uint32_t value) {
    std::cout << "    " << name << ": 0x" << std::hex << std::setw(8)
              << std::setfill('0') << value << std::dec << std::setfill(' ')
              << "  plugs=" << (value & 0x1f) << '\n';
}

void printPcr(const char* name, std::uint32_t value, bool output) {
    const bool online = (value & 0x80000000u) != 0;
    const bool broadcast = (value & 0x40000000u) != 0;
    const unsigned p2p = (value >> 24) & 0x3f;
    const unsigned channel = (value >> 16) & 0x3f;

    std::cout << "    " << name << ": 0x" << std::hex << std::setw(8)
              << std::setfill('0') << value << std::dec << std::setfill(' ')
              << "  online=" << (online ? "yes" : "no")
              << " p2p=" << p2p
              << " broadcast=" << (broadcast ? "yes" : "no")
              << " channel=" << channel;
    if (output) {
        const unsigned speed = (value >> 14) & 0x3;
        const unsigned xspeed = (value >> 22) & 0x3;
        std::cout << " speed=" << speed << " xspeed=" << xspeed;
    }
    std::cout << '\n';
}

void printCmp(IOFireWireLibDeviceRef device, UInt32 generation, UInt16 node) {
    std::uint32_t ompr = 0, opcr0 = 0, impr = 0, ipcr0 = 0;
    std::cout << "    CMP register snapshot:\n";

    const bool omprOk = readQuadlet(device, generation, node, kOmprLo, ompr);
    const bool opcrOk = readQuadlet(device, generation, node, kOpcr0Lo, opcr0);
    const bool imprOk = readQuadlet(device, generation, node, kImprLo, impr);
    const bool ipcrOk = readQuadlet(device, generation, node, kIpcr0Lo, ipcr0);

    if (omprOk) printMpr("oMPR", ompr);
    if (opcrOk) printPcr("oPCR[0] device OUTPUT / host capture", opcr0, true);
    if (imprOk) printMpr("iMPR", impr);
    if (ipcrOk) printPcr("iPCR[0] device INPUT / host playback", ipcr0, false);
}

void usage(const char* argv0) {
    std::cout << "usage: " << argv0
              << " --product <FireWire Product Name>"
              << " [--maudio-special] [--bridgeco-formats] [--raw]\n"
              << "  default             standard signal-format STATUS + CMP reads\n"
              << "  --maudio-special    use the restricted M-Audio special-firmware path\n"
              << "  --bridgeco-formats  opt in to BridgeCo extended format enumeration\n"
              << "  --raw               print raw FCP commands/responses\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string product;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--product" && i + 1 < argc)
            product = argv[++i];
        else if (arg == "--maudio-special")
            gMaudioSpecial = true;
        else if (arg == "--bridgeco-formats")
            gBridgeCoFormats = true;
        else if (arg == "--raw")
            gRaw = true;
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 64;
        }
    }

    if (product.empty() || (gMaudioSpecial && gBridgeCoFormats)) {
        usage(argv[0]);
        return 64;
    }

    std::cout << "macfw bebobprobe — observational BeBoB operational probe\n"
              << "target product: " << product << '\n';
    if (gMaudioSpecial)
        std::cout << "mode: M-Audio special firmware (no BridgeCo extensions)\n";
    std::cout << '\n';

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;

    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching,
                                     &iterator) != KERN_SUCCESS)
        return 1;

    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        const std::string foundProduct =
            stringProperty(service, "FireWire Product Name");
        if (foundProduct != product) {
            IOObjectRelease(service);
            continue;
        }

        std::uint64_t vendor = 0, guid = 0, spec = 0, sw = 0;
        numberProperty(service, "Vendor_ID", vendor);
        numberProperty(service, "GUID", guid);
        numberProperty(service, "Unit_Spec_ID", spec);
        numberProperty(service, "Unit_SW_Version", sw);

        std::cout << "matched operational unit:\n"
                  << "    product: " << foundProduct << '\n'
                  << "    vendor:  0x" << std::hex << vendor << '\n'
                  << "    GUID:    0x" << guid << '\n'
                  << "    spec:    0x" << spec << '\n'
                  << "    SW:      0x" << sw << std::dec << '\n';

        IOCFPlugInInterface** plugin = nullptr;
        SInt32 score = 0;
        const kern_return_t kr = IOCreatePlugInInterfaceForService(
            service, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID,
            &plugin, &score);
        IOObjectRelease(service);
        if (kr != KERN_SUCCESS || !plugin) {
            IOObjectRelease(iterator);
            return 2;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID*>(&device));
        IODestroyPlugInInterface(plugin);
        if (hr != 0 || !device) {
            IOObjectRelease(iterator);
            return 2;
        }

        UInt32 generation = 0;
        UInt16 node = 0;
        if ((*device)->GetBusGeneration(device, &generation) != kIOReturnSuccess ||
            (*device)->GetRemoteNodeID(device, generation, &node) != kIOReturnSuccess) {
            (*device)->Release(device);
            IOObjectRelease(iterator);
            return 3;
        }

        std::cout << "    generation: " << generation << '\n'
                  << "    remote node: 0x" << std::hex << node
                  << std::dec << '\n';

        if ((*device)->Open(device) != kIOReturnSuccess) {
            (*device)->Release(device);
            IOObjectRelease(iterator);
            return 4;
        }

        // Read CMP before sending any FCP transaction. This makes transport
        // register health independently visible when a device has AV/C issues.
        printCmp(device, generation, node);

        if ((*device)->AddCallbackDispatcherToRunLoop(
                device, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
            (*device)->Close(device);
            (*device)->Release(device);
            IOObjectRelease(iterator);
            return 4;
        }

        ResponseContext ctx;
        ctx.expectedNode = node;
        auto responseSpace = (*device)->CreateInitialUnitsPseudoAddressSpace(
            device, kFcpResponseLo, kFcpResponseSize, &ctx, 1024, nullptr,
            kFWAddressSpaceNoReadAccess | kFWAddressSpaceShareIfExists,
            CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
        if (!responseSpace) {
            (*device)->RemoveCallbackDispatcherFromRunLoop(device);
            (*device)->Close(device);
            (*device)->Release(device);
            IOObjectRelease(iterator);
            return 5;
        }
        (*responseSpace)->SetWriteHandler(responseSpace, responseHandler);
        (*responseSpace)->TurnOnNotification(responseSpace);

        if (gMaudioSpecial) {
            // Linux snd-bebob treats the INPUT plug as the authoritative rate
            // source for M-Audio special firmware and does not query BridgeCo
            // stream-format extensions on FW1814/ProjectMix.
            unsigned inRate = 0;
            bool inOk = false;
            for (unsigned attempt = 0; attempt < 3 && !inOk; ++attempt)
                inOk = readSignalRate(device, generation, node, ctx,
                                      0x19, inRate);

            std::cout << "    current signal format:\n"
                      << "        device INPUT / host playback: "
                      << (inOk ? std::to_string(inRate) : "unknown")
                      << " Hz\n"
                      << "        device OUTPUT / host capture: not queried"
                      << " (special-firmware quirk)\n"
                      << "    supported stream formats:\n"
                      << "        not queried: FW1814 special firmware does not"
                      << " support BridgeCo extensions\n";
        } else {
            unsigned outRate = 0, inRate = 0;
            const bool outOk = readSignalRate(device, generation, node, ctx,
                                              0x18, outRate);
            const bool inOk = readSignalRate(device, generation, node, ctx,
                                             0x19, inRate);
            std::cout << "    current signal formats:\n"
                      << "        device OUTPUT / host capture:  "
                      << (outOk ? std::to_string(outRate) : "unknown")
                      << " Hz\n"
                      << "        device INPUT / host playback: "
                      << (inOk ? std::to_string(inRate) : "unknown")
                      << " Hz\n";

            if (gBridgeCoFormats) {
                enumerateFormats(device, generation, node, ctx,
                                 0x01, "device OUTPUT / host capture");
                enumerateFormats(device, generation, node, ctx,
                                 0x00, "device INPUT / host playback");
            } else {
                std::cout << "    BridgeCo stream-format enumeration: skipped"
                          << " (opt in with --bridgeco-formats)\n";
            }
        }

        (*responseSpace)->TurnOffNotification(responseSpace);
        (*responseSpace)->Release(responseSpace);
        (*device)->RemoveCallbackDispatcherFromRunLoop(device);
        (*device)->Close(device);
        (*device)->Release(device);
        IOObjectRelease(iterator);
        return 0;
    }

    IOObjectRelease(iterator);
    std::cout << "No matching operational unit found.\n";
    return 2;
}
