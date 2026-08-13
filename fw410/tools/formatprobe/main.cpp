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
constexpr double kTimeoutSeconds = 1.0;
constexpr unsigned kMaxEntries = 16;

struct ResponseContext {
    UInt16 expectedNode = 0;
    bool received = false;
    UInt16 sourceNode = 0;
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

static void printProperty(io_registry_entry_t service, const char *key) {
    CFStringRef keyString = CFStringCreateWithCString(
        kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!keyString) return;
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, keyString, kCFAllocatorDefault, 0);
    CFRelease(keyString);
    if (!value) return;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        char buffer[1024] = {};
        if (CFStringGetCString(static_cast<CFStringRef>(value), buffer,
                               sizeof(buffer), kCFStringEncodingUTF8))
            std::cout << "    " << key << ": " << buffer << '\n';
    } else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        long long n = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &n);
        std::cout << "    " << key << ": 0x" << std::hex << n << std::dec << '\n';
    }
    CFRelease(value);
}

static void printBytes(const UInt8 *bytes, UInt32 length) {
    for (UInt32 i = 0; i < length; ++i) {
        if (i) std::cout << ' ';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(bytes[i]);
    }
    std::cout << std::dec << std::setfill(' ');
}

static const char *responseName(UInt8 r) {
    switch (r) {
        case 0x08: return "NOT IMPLEMENTED";
        case 0x0a: return "REJECTED";
        case 0x0b: return "IN TRANSITION";
        case 0x0c: return "IMPLEMENTED/STABLE";
        default: return "other";
    }
}

static IOReturn writeAbsolute(IOFireWireLibDeviceRef device, UInt32 generation,
                              UInt16 remoteNodeID, const void *buffer, UInt32 &size) {
    FWAddress address = {};
    address.nodeID = remoteNodeID;
    address.addressHi = kAddressHi;
    address.addressLo = kFcpCommandLo;
    return (*device)->Write(device, 0, &address, buffer, &size, true, generation);
}

static UInt32 fcpWriteHandler(IOFireWireLibPseudoAddressSpaceRef addressSpace,
                              FWClientCommandID commandID, UInt32 packetLen,
                              void *packet, UInt16 srcNodeID, UInt32, UInt32,
                              void *refCon) {
    auto *ctx = static_cast<ResponseContext *>(refCon);
    if (ctx && packet && srcNodeID == ctx->expectedNode) {
        const UInt32 copyLen = std::min<UInt32>(packetLen, ctx->bytes.size());
        std::memcpy(ctx->bytes.data(), packet, copyLen);
        ctx->sourceNode = srcNodeID;
        ctx->length = copyLen;
        ctx->received = true;
    }
    (*addressSpace)->ClientCommandIsComplete(addressSpace, commandID, kIOReturnSuccess);
    return kIOReturnSuccess;
}

static void resetResponse(ResponseContext &ctx) {
    ctx.received = false;
    ctx.sourceNode = 0;
    ctx.length = 0;
    ctx.bytes.fill(0);
}

static std::array<UInt8, 12> formatCommand(UInt8 direction, UInt8 eid) {
    // BridgeCo STREAM FORMAT SUPPORT / list request for unit isochronous plug 0.
    // Address bytes: unit=ff, dir, mode=00, unit-type=00, plug=00, reserved=ff.
    return {0x01, 0xff, 0x2f, 0xc1,
            direction, 0x00, 0x00, 0x00, 0xff,
            0x00, eid, 0x00};
}

static bool transaction(IOFireWireLibDeviceRef device, UInt32 generation,
                        UInt16 remoteNodeID, ResponseContext &ctx,
                        const std::array<UInt8, 12> &command) {
    resetResponse(ctx);
    UInt32 size = static_cast<UInt32>(command.size());
    const IOReturn kr = writeAbsolute(device, generation, remoteNodeID,
                                      command.data(), size);
    if (kr != kIOReturnSuccess) {
        std::cout << "        write failed: 0x" << std::hex << kr << std::dec << '\n';
        return false;
    }
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
    while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    return ctx.received;
}

static void enumerateDirection(IOFireWireLibDeviceRef device, UInt32 generation,
                               UInt16 remoteNodeID, ResponseContext &ctx,
                               UInt8 direction, const char *name) {
    std::cout << "    BridgeCo " << name << " stream-format list:\n";
    for (unsigned eid = 0; eid < kMaxEntries; ++eid) {
        const auto command = formatCommand(direction, static_cast<UInt8>(eid));
        std::cout << "        entry " << eid << " command: ";
        printBytes(command.data(), command.size());
        std::cout << '\n';

        if (!transaction(device, generation, remoteNodeID, ctx, command)) {
            std::cout << "        response: timeout\n";
            break;
        }

        std::cout << "        response (" << ctx.length << " bytes): ";
        printBytes(ctx.bytes.data(), ctx.length);
        std::cout << '\n';
        if (!ctx.length) break;

        std::cout << "        AV/C response: 0x" << std::hex
                  << static_cast<unsigned>(ctx.bytes[0]) << std::dec
                  << " (" << responseName(ctx.bytes[0]) << ")\n";

        if (ctx.bytes[0] != 0x0c) {
            std::cout << "        end of list / entry unavailable\n";
            break;
        }
        if (ctx.length < 12 || ctx.bytes[10] != eid) {
            std::cout << "        response shape: unexpected\n";
            break;
        }

        std::cout << "        format payload [11..]: ";
        printBytes(ctx.bytes.data() + 11, ctx.length - 11);
        std::cout << '\n';
    }
}

static bool runProbe(IOFireWireLibDeviceRef device, UInt32 generation,
                     UInt16 remoteNodeID) {
    ResponseContext ctx;
    ctx.expectedNode = remoteNodeID;

    const IOReturn openResult = (*device)->Open(device);
    if (openResult != kIOReturnSuccess) {
        std::cout << "    open failed: 0x" << std::hex << openResult << std::dec << '\n';
        return false;
    }
    const IOReturn dispatcherResult =
        (*device)->AddCallbackDispatcherToRunLoop(device, CFRunLoopGetCurrent());
    if (dispatcherResult != kIOReturnSuccess) {
        (*device)->Close(device);
        return false;
    }

    IOFireWireLibPseudoAddressSpaceRef responseSpace =
        (*device)->CreateInitialUnitsPseudoAddressSpace(
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

    enumerateDirection(device, generation, remoteNodeID, ctx, 0x01, "OUTPUT");
    enumerateDirection(device, generation, remoteNodeID, ctx, 0x00, "INPUT");

    (*responseSpace)->TurnOffNotification(responseSpace);
    (*responseSpace)->Release(responseSpace);
    (*device)->RemoveCallbackDispatcherFromRunLoop(device);
    (*device)->Close(device);
    return true;
}

} // namespace

int main() {
    std::cout << "macfw formatprobe — BridgeCo/BeBoB stream-format enumeration\n\n";

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
    if (kr != KERN_SUCCESS) return 1;

    bool found = false;
    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        if (!isOperationalFw410(service)) {
            IOObjectRelease(service);
            continue;
        }
        found = true;
        std::cout << "FW410 operational unit:\n";
        printProperty(service, "FireWire Product Name");
        printProperty(service, "GUID");

        IOCFPlugInInterface **plugin = nullptr;
        SInt32 score = 0;
        kr = IOCreatePlugInInterfaceForService(service, kIOFireWireLibTypeID,
                                               kIOCFPlugInInterfaceID, &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) {
            IOObjectRelease(service);
            continue;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID *>(&device));
        if (hr == 0 && device) {
            UInt32 generation = 0;
            UInt16 nodeID = 0;
            if ((*device)->GetBusGeneration(device, &generation) == kIOReturnSuccess &&
                (*device)->GetRemoteNodeID(device, generation, &nodeID) == kIOReturnSuccess) {
                std::cout << "    generation: " << generation << '\n';
                std::cout << "    remote node: 0x" << std::hex << nodeID << std::dec << '\n';
                runProbe(device, generation, nodeID);
            }
            (*device)->Release(device);
        }
        IODestroyPlugInInterface(plugin);
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);

    if (!found) {
        std::cout << "No operational FW 410 unit found.\n";
        return 2;
    }
    return 0;
}
