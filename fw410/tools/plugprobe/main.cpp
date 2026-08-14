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

static std::array<UInt8,12> commandFor(UInt8 dir, UInt8 plug, UInt8 infoType) {
    // BridgeCo GENERAL PLUG INFO extension for UNIT external plug.
    // addr = {UNIT, dir, UNIT mode, EXT, plug, reserved}
    return {
        0x01, 0xff, 0x02, 0xc0,
        dir, 0x00, 0x01, plug, 0xff,
        infoType,
        0xff, 0xff
    };
}

static bool transact(IOFireWireLibDeviceRef device,
                     UInt32 generation,
                     UInt16 remoteNodeID,
                     ResponseContext& ctx,
                     const std::array<UInt8,12>& cmd) {
    reset(ctx);
    FWAddress address{};
    address.nodeID = remoteNodeID;
    address.addressHi = kAddressHi;
    address.addressLo = kFcpCommandLo;
    UInt32 size = static_cast<UInt32>(cmd.size());
    if ((*device)->Write(device, 0, &address, cmd.data(), &size,
                         true, generation) != kIOReturnSuccess)
        return false;

    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
    while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    return ctx.received;
}

static const char* plugTypeName(UInt8 type) {
    switch (type) {
        case 0x00: return "isochronous";
        case 0x01: return "asynchronous";
        case 0x02: return "MIDI";
        case 0x03: return "sync";
        case 0x04: return "analog";
        case 0x05: return "digital";
        case 0x06: return "additional/internal";
        default: return "unknown";
    }
}

static bool queryByte(IOFireWireLibDeviceRef device,
                      UInt32 generation,
                      UInt16 node,
                      ResponseContext& ctx,
                      UInt8 dir,
                      UInt8 plug,
                      UInt8 infoType,
                      UInt8& value) {
    const auto cmd = commandFor(dir, plug, infoType);
    if (!transact(device, generation, node, ctx, cmd)) return false;
    if (ctx.length < 11 || ctx.bytes[0] != 0x0c) return false;
    value = ctx.bytes[10];
    return true;
}

static bool runProbe(IOFireWireLibDeviceRef device,
                     UInt32 generation,
                     UInt16 remoteNodeID) {
    ResponseContext ctx;
    ctx.expectedNode = remoteNodeID;

    if ((*device)->Open(device) != kIOReturnSuccess) return false;
    if ((*device)->AddCallbackDispatcherToRunLoop(device, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
        (*device)->Close(device);
        return false;
    }

    auto responseSpace = (*device)->CreateInitialUnitsPseudoAddressSpace(
        device, kFcpResponseLo, kFcpResponseSize, &ctx, 1024, nullptr,
        kFWAddressSpaceNoReadAccess | kFWAddressSpaceShareIfExists,
        CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
    if (!responseSpace) {
        (*device)->RemoveCallbackDispatcherFromRunLoop(device);
        (*device)->Close(device);
        return false;
    }

    (*responseSpace)->SetWriteHandler(responseSpace, fcpWriteHandler);
    if (!(*responseSpace)->TurnOnNotification(responseSpace)) {
        (*responseSpace)->Release(responseSpace);
        (*device)->RemoveCallbackDispatcherFromRunLoop(device);
        (*device)->Close(device);
        return false;
    }

    std::cout << "    External output plugs (read-only BridgeCo plug info):\n";
    for (UInt8 plug = 0; plug < 16; ++plug) {
        UInt8 type = 0xff;
        UInt8 channels = 0xff;
        const bool hasType = queryByte(device, generation, remoteNodeID, ctx,
                                       0x01, plug, 0x00, type);
        const bool hasChannels = queryByte(device, generation, remoteNodeID, ctx,
                                           0x01, plug, 0x02, channels);
        if (!hasType && !hasChannels) continue;

        std::cout << "        external OUT plug " << static_cast<unsigned>(plug) << ':';
        if (hasType)
            std::cout << " type=" << plugTypeName(type)
                      << " (0x" << std::hex << static_cast<unsigned>(type) << std::dec << ')';
        if (hasChannels)
            std::cout << " channels=" << static_cast<unsigned>(channels);
        std::cout << '\n';
    }

    (*responseSpace)->TurnOffNotification(responseSpace);
    (*responseSpace)->Release(responseSpace);
    (*device)->RemoveCallbackDispatcherFromRunLoop(device);
    (*device)->Close(device);
    return true;
}
}

int main() {
    std::cout << "macfw plugprobe — read-only FW410 external output plug probe\n\n";

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS)
        return 1;

    bool found = false;
    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        if (!isOperationalFw410(service)) {
            IOObjectRelease(service);
            continue;
        }
        found = true;

        IOCFPlugInInterface** plugin = nullptr;
        SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(service, kIOFireWireLibTypeID,
                kIOCFPlugInInterfaceID, &plugin, &score) != KERN_SUCCESS || !plugin) {
            IOObjectRelease(service);
            continue;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(plugin,
            CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID*>(&device));
        if (hr != 0 || !device) {
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            continue;
        }

        UInt32 generation = 0;
        UInt16 node = 0;
        (*device)->GetBusGeneration(device, &generation);
        (*device)->GetRemoteNodeID(device, generation, &node);

        std::cout << "FW410 operational unit:\n"
                  << "    generation: " << generation << '\n'
                  << "    remote node: 0x" << std::hex << node << std::dec << '\n';
        runProbe(device, generation, node);

        (*device)->Release(device);
        IODestroyPlugInInterface(plugin);
        IOObjectRelease(service);
        break;
    }

    IOObjectRelease(iterator);
    if (!found) {
        std::cout << "No operational FW 410 unit found.\n";
        return 1;
    }
    return 0;
}
