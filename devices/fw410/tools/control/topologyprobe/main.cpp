#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

namespace {
constexpr UInt16 kAddressHi = 0xffff;
constexpr UInt32 kFcpCommandLo = 0xf0000b00;
constexpr UInt32 kFcpResponseLo = 0xf0000d00;
constexpr UInt32 kFcpResponseSize = 0x200;
constexpr double kTimeoutSeconds = 0.25;

struct ResponseContext {
    UInt16 expectedNode = 0;
    bool received = false;
    UInt32 length = 0;
    std::array<UInt8, kFcpResponseSize> bytes{};
};

static bool isOperationalFw410(io_registry_entry_t service) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, CFSTR("FireWire Product Name"), kCFAllocatorDefault, 0);
    if (!value) return false;
    const bool result = CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringCompare(static_cast<CFStringRef>(value), CFSTR("FW 410"), 0) == kCFCompareEqualTo;
    CFRelease(value);
    return result;
}

static void printBytes(const UInt8* bytes, UInt32 length) {
    for (UInt32 i = 0; i < length; ++i) {
        if (i) std::cout << ' ';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(bytes[i]);
    }
    std::cout << std::dec << std::setfill(' ');
}

static UInt32 fcpWriteHandler(IOFireWireLibPseudoAddressSpaceRef addressSpace,
                              FWClientCommandID commandID,
                              UInt32 packetLen,
                              void* packet,
                              UInt16 srcNodeID,
                              UInt32, UInt32,
                              void* refCon) {
    auto* ctx = static_cast<ResponseContext*>(refCon);
    if (ctx && packet && srcNodeID == ctx->expectedNode) {
        const UInt32 n = std::min<UInt32>(packetLen, ctx->bytes.size());
        std::memcpy(ctx->bytes.data(), packet, n);
        ctx->length = n;
        ctx->received = true;
    }
    (*addressSpace)->ClientCommandIsComplete(addressSpace, commandID, kIOReturnSuccess);
    return kIOReturnSuccess;
}

static void reset(ResponseContext& ctx) {
    ctx.received = false;
    ctx.length = 0;
    ctx.bytes.fill(0);
}

static bool transact(IOFireWireLibDeviceRef device,
                     UInt32 generation,
                     UInt16 remoteNodeID,
                     ResponseContext& ctx,
                     const std::array<UInt8, 16>& cmd,
                     bool raw) {
    reset(ctx);
    FWAddress address{};
    address.nodeID = remoteNodeID;
    address.addressHi = kAddressHi;
    address.addressLo = kFcpCommandLo;
    UInt32 size = static_cast<UInt32>(cmd.size());
    const IOReturn kr = (*device)->Write(device, 0, &address,
                                        cmd.data(), &size, true, generation);
    if (kr != kIOReturnSuccess) return false;

    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
    while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    if (!ctx.received) return false;

    if (raw) {
        std::cout << "        command:  ";
        printBytes(cmd.data(), static_cast<UInt32>(cmd.size()));
        std::cout << "\n        response: ";
        printBytes(ctx.bytes.data(), ctx.length);
        std::cout << '\n';
    }
    return true;
}

static std::array<UInt8,16> plugInputCommand(const std::array<UInt8,6>& addr) {
    return {
        0x01, addr[0], 0x02, 0xc0,
        addr[1], addr[2], addr[3], addr[4], addr[5],
        0x05,
        0xff,0xff,0xff,0xff,0xff,0xff
    };
}

static const char* dirName(UInt8 d) {
    return d == 0x00 ? "IN" : d == 0x01 ? "OUT" : "?";
}
static const char* modeName(UInt8 m) {
    switch (m) {
        case 0x00: return "unit";
        case 0x01: return "subunit";
        case 0x02: return "function-block";
        default: return "unknown";
    }
}
static const char* subunitTypeName(UInt8 t) {
    switch (t) {
        case 0x01: return "Audio";
        case 0x0c: return "Music";
        default: return "unknown";
    }
}
static const char* fbTypeName(UInt8 t) {
    switch (t) {
        case 0x80: return "selector";
        case 0x81: return "feature";
        case 0x82: return "processing";
        case 0x83: return "codec";
        default: return "unknown";
    }
}

static void printAddress(const UInt8* a, std::size_t n) {
    if (n < 4) {
        printBytes(a, static_cast<UInt32>(n));
        return;
    }

    std::cout << "dir=" << dirName(a[0]) << " mode=" << modeName(a[1]);

    if (a[1] == 0x00) {
        std::cout << " unit-type=0x" << std::hex << static_cast<unsigned>(a[2])
                  << std::dec << " plug=" << static_cast<unsigned>(a[3]);
        return;
    }

    if (a[1] == 0x01) {
        if (n < 5) { std::cout << " truncated"; return; }
        std::cout << ' ' << subunitTypeName(a[2])
                  << "-subunit=" << static_cast<unsigned>(a[3])
                  << " plug=" << static_cast<unsigned>(a[4]);
        return;
    }

    if (a[1] == 0x02) {
        // BridgeCo function-block source address is seven bytes:
        // dir, mode, subunit-type, subunit-id, fb-type, fb-id, plug-id.
        if (n < 7) { std::cout << " truncated"; return; }
        std::cout << ' ' << subunitTypeName(a[2])
                  << "-subunit=" << static_cast<unsigned>(a[3])
                  << ' ' << fbTypeName(a[4]) << "-fb="
                  << static_cast<unsigned>(a[5])
                  << " plug=" << static_cast<unsigned>(a[6]);
        return;
    }
}

static bool queryAndPrint(IOFireWireLibDeviceRef device,
                          UInt32 generation,
                          UInt16 node,
                          ResponseContext& ctx,
                          const std::string& label,
                          const std::array<UInt8,6>& addr,
                          bool raw) {
    const auto cmd = plugInputCommand(addr);
    if (!transact(device, generation, node, ctx, cmd, raw)) return false;
    if (ctx.length < 1 || ctx.bytes[0] != 0x0c) return false;

    std::cout << "        " << label << " <- ";
    if (ctx.length >= 14)
        printAddress(ctx.bytes.data() + 10, ctx.length - 10);
    else
        std::cout << "short response";
    std::cout << '\n';
    return true;
}

static void scanUnit(IOFireWireLibDeviceRef device, UInt32 gen, UInt16 node,
                     ResponseContext& ctx, bool raw) {
    std::cout << "    Unit plug-input topology:\n";
    for (UInt8 dir = 0; dir <= 1; ++dir) {
        for (UInt8 unitType = 0; unitType <= 2; ++unitType) {
            for (UInt8 plug = 0; plug < 16; ++plug) {
                std::array<UInt8,6> addr = {0xff, dir, 0x00, unitType, plug, 0xff};
                const std::string label = std::string(dirName(dir)) +
                    " unit-type 0x" + "0123456789abcdef"[unitType] +
                    " plug " + std::to_string(plug);
                queryAndPrint(device, gen, node, ctx, label, addr, raw);
            }
        }
    }
}

static void scanSubunit(IOFireWireLibDeviceRef device, UInt32 gen, UInt16 node,
                        ResponseContext& ctx, UInt8 subunitByte,
                        const char* name, bool raw) {
    std::cout << "    " << name << " Subunit plug-input topology:\n";
    for (UInt8 dir = 0; dir <= 1; ++dir) {
        for (UInt8 plug = 0; plug < 16; ++plug) {
            std::array<UInt8,6> addr = {subunitByte, dir, 0x01, plug, 0xff, 0xff};
            const std::string label = std::string(dirName(dir)) + ' ' + name +
                " plug " + std::to_string(plug);
            queryAndPrint(device, gen, node, ctx, label, addr, raw);
        }
    }
}

static void scanFunctionBlocks(IOFireWireLibDeviceRef device, UInt32 gen, UInt16 node,
                               ResponseContext& ctx, bool raw) {
    std::cout << "    Audio function-block plug-input topology:\n";
    const std::array<UInt8,3> types = {0x80, 0x81, 0x82};
    for (UInt8 type : types) {
        const unsigned maxFb = type == 0x80 ? 7 : type == 0x81 ? 15 : 8;
        for (UInt8 dir = 0; dir <= 1; ++dir) {
            for (unsigned fb = 1; fb <= maxFb; ++fb) {
                for (UInt8 plug = 0; plug < 4; ++plug) {
                    std::array<UInt8,6> addr = {
                        0x08, dir, 0x02, type,
                        static_cast<UInt8>(fb), plug
                    };
                    const std::string label = std::string(fbTypeName(type)) +
                        " fb " + std::to_string(fb) + " " + dirName(dir) +
                        " plug " + std::to_string(plug);
                    queryAndPrint(device, gen, node, ctx, label, addr, raw);
                }
            }
        }
    }
}

static bool runProbe(IOFireWireLibDeviceRef device, UInt32 generation,
                     UInt16 remoteNodeID, bool raw) {
    ResponseContext ctx;
    ctx.expectedNode = remoteNodeID;
    if ((*device)->Open(device) != kIOReturnSuccess) return false;
    if ((*device)->AddCallbackDispatcherToRunLoop(device, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
        (*device)->Close(device); return false;
    }
    auto responseSpace = (*device)->CreateInitialUnitsPseudoAddressSpace(
        device, kFcpResponseLo, kFcpResponseSize, &ctx, 1024, nullptr,
        kFWAddressSpaceNoReadAccess | kFWAddressSpaceShareIfExists,
        CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
    if (!responseSpace) {
        (*device)->RemoveCallbackDispatcherFromRunLoop(device); (*device)->Close(device); return false;
    }
    (*responseSpace)->SetWriteHandler(responseSpace, fcpWriteHandler);
    if (!(*responseSpace)->TurnOnNotification(responseSpace)) {
        (*responseSpace)->Release(responseSpace);
        (*device)->RemoveCallbackDispatcherFromRunLoop(device); (*device)->Close(device); return false;
    }

    scanUnit(device, generation, remoteNodeID, ctx, raw);
    scanSubunit(device, generation, remoteNodeID, ctx, 0x08, "Audio", raw);
    scanSubunit(device, generation, remoteNodeID, ctx, 0x60, "Music", raw);
    scanFunctionBlocks(device, generation, remoteNodeID, ctx, raw);

    std::cout << "    note: STATUS queries only; no routing or mixer values are changed\n";
    (*responseSpace)->TurnOffNotification(responseSpace);
    (*responseSpace)->Release(responseSpace);
    (*device)->RemoveCallbackDispatcherFromRunLoop(device);
    (*device)->Close(device);
    return true;
}
}

int main(int argc, char** argv) {
    bool raw = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--raw") raw = true;
        else { std::cerr << "usage: ./topologyprobe [--raw]\n"; return 64; }
    }
    std::cout << "macfw topologyprobe — read-only FW410 BridgeCo routing topology probe\n\n";
    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS) return 1;
    bool found = false;
    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        if (!isOperationalFw410(service)) { IOObjectRelease(service); continue; }
        found = true;
        IOCFPlugInInterface** plugin = nullptr;
        SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(service, kIOFireWireLibTypeID,
                kIOCFPlugInInterfaceID, &plugin, &score) != KERN_SUCCESS || !plugin) {
            IOObjectRelease(service); continue;
        }
        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(plugin,
            CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID*>(&device));
        if (hr != 0 || !device) {
            IODestroyPlugInInterface(plugin); IOObjectRelease(service); continue;
        }
        UInt32 generation = 0; UInt16 node = 0;
        (*device)->GetBusGeneration(device, &generation);
        (*device)->GetRemoteNodeID(device, generation, &node);
        std::cout << "FW410 operational unit:\n"
                  << "    generation: " << generation << '\n'
                  << "    remote node: 0x" << std::hex << node << std::dec << '\n';
        runProbe(device, generation, node, raw);
        (*device)->Release(device);
        IODestroyPlugInInterface(plugin);
        IOObjectRelease(service);
        break;
    }
    IOObjectRelease(iterator);
    if (!found) { std::cout << "No operational FW 410 unit found.\n"; return 1; }
    return 0;
}
