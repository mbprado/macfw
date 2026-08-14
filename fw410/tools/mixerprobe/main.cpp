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
constexpr unsigned kMaxFunctionBlockId = 31;

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
        CFStringCompare(static_cast<CFStringRef>(value),
                        CFSTR("FW 410"), 0) == kCFCompareEqualTo;
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

static const char* responseName(UInt8 response) {
    switch (response) {
        case 0x08: return "NOT IMPLEMENTED";
        case 0x09: return "ACCEPTED";
        case 0x0a: return "REJECTED";
        case 0x0b: return "IN TRANSITION";
        case 0x0c: return "IMPLEMENTED/STABLE";
        case 0x0d: return "CHANGED";
        case 0x0f: return "INTERIM";
        default: return "unknown";
    }
}

static UInt32 fcpWriteHandler(IOFireWireLibPseudoAddressSpaceRef addressSpace,
                              FWClientCommandID commandID,
                              UInt32 packetLen,
                              void* packet,
                              UInt16 srcNodeID,
                              UInt32,
                              UInt32,
                              void* refCon) {
    auto* ctx = static_cast<ResponseContext*>(refCon);
    if (ctx && packet && srcNodeID == ctx->expectedNode) {
        const UInt32 copyLen = std::min<UInt32>(packetLen, ctx->bytes.size());
        std::memcpy(ctx->bytes.data(), packet, copyLen);
        ctx->sourceNode = srcNodeID;
        ctx->length = copyLen;
        ctx->received = true;
    }
    (*addressSpace)->ClientCommandIsComplete(addressSpace, commandID,
                                             kIOReturnSuccess);
    return kIOReturnSuccess;
}

static void resetResponse(ResponseContext& ctx) {
    ctx.received = false;
    ctx.sourceNode = 0;
    ctx.length = 0;
    ctx.bytes.fill(0);
}

static IOReturn writeFcp(IOFireWireLibDeviceRef device,
                         UInt32 generation,
                         UInt16 remoteNodeID,
                         const void* buffer,
                         UInt32& size) {
    FWAddress address = {};
    address.nodeID = remoteNodeID;
    address.addressHi = kAddressHi;
    address.addressLo = kFcpCommandLo;
    return (*device)->Write(device, 0, &address, buffer, &size,
                            true, generation);
}

static std::array<UInt8, 12> selectorStatusCommand(UInt8 subunitId,
                                                   UInt8 fbId) {
    // AV/C Audio Subunit FUNCTION BLOCK, selector, CURRENT attribute.
    // Read-only STATUS query, matching Linux avc_audio_get_selector().
    return {
        0x01,                              // AV/C STATUS
        static_cast<UInt8>(0x08 | (subunitId & 0x07)),
        0xb8,                              // FUNCTION BLOCK
        0x80,                              // selector function block
        fbId,                              // function block ID
        0x10,                              // CURRENT
        0x02,                              // selector data length
        0xff,                              // queried input plug number
        0x01,                              // SELECTOR_CONTROL
        0x00, 0x00, 0x00
    };
}

static bool transaction(IOFireWireLibDeviceRef device,
                        UInt32 generation,
                        UInt16 remoteNodeID,
                        ResponseContext& ctx,
                        const std::array<UInt8, 12>& command,
                        bool raw) {
    resetResponse(ctx);
    UInt32 size = static_cast<UInt32>(command.size());
    const IOReturn kr = writeFcp(device, generation, remoteNodeID,
                                 command.data(), size);
    if (kr != kIOReturnSuccess)
        return false;

    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
    while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);

    if (!ctx.received)
        return false;

    if (raw) {
        std::cout << "        command:  ";
        printBytes(command.data(), static_cast<UInt32>(command.size()));
        std::cout << "\n        response: ";
        printBytes(ctx.bytes.data(), ctx.length);
        std::cout << '\n';
    }
    return true;
}

static bool runProbe(IOFireWireLibDeviceRef device,
                     UInt32 generation,
                     UInt16 remoteNodeID,
                     bool raw) {
    ResponseContext ctx;
    ctx.expectedNode = remoteNodeID;

    const IOReturn openResult = (*device)->Open(device);
    if (openResult != kIOReturnSuccess) {
        std::cout << "    open: failed (0x" << std::hex << openResult
                  << std::dec << ")\n";
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

    std::cout << "    Audio subunit 0 selector function blocks (read-only):\n";

    unsigned implemented = 0;
    unsigned responded = 0;
    for (unsigned fb = 0; fb <= kMaxFunctionBlockId; ++fb) {
        const auto command = selectorStatusCommand(0, static_cast<UInt8>(fb));
        if (!transaction(device, generation, remoteNodeID, ctx, command, raw))
            continue;

        ++responded;
        if (ctx.length < 9)
            continue;

        const UInt8 response = ctx.bytes[0];
        if (response != 0x0c)
            continue;

        ++implemented;
        std::cout << "        selector fb " << fb
                  << ": current input plug="
                  << static_cast<unsigned>(ctx.bytes[7])
                  << " response=" << responseName(response) << '\n';
    }

    if (!implemented) {
        std::cout << "        no implemented selector blocks found in IDs 0.."
                  << kMaxFunctionBlockId << '\n';
    }
    std::cout << "    selector responses: " << responded
              << ", implemented: " << implemented << '\n';
    std::cout << "    note: this probe sends AV/C STATUS only; it performs no CONTROL writes\n";

    (*responseSpace)->TurnOffNotification(responseSpace);
    (*responseSpace)->Release(responseSpace);
    (*device)->RemoveCallbackDispatcherFromRunLoop(device);
    (*device)->Close(device);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool raw = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--raw") raw = true;
        else {
            std::cerr << "usage: ./mixerprobe [--raw]\n";
            return 64;
        }
    }

    std::cout << "macfw mixerprobe — read-only FW410 AV/C mixer selector probe\n\n";

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;

    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                    matching, &iterator);
    if (kr != KERN_SUCCESS) return 1;

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
        kr = IOCreatePlugInInterfaceForService(service,
                                               kIOFireWireLibTypeID,
                                               kIOCFPlugInInterfaceID,
                                               &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) {
            IOObjectRelease(service);
            continue;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin,
            CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID*>(&device));
        if (hr != 0 || !device) {
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            continue;
        }

        UInt32 generation = 0;
        UInt16 nodeID = 0;
        (*device)->GetBusGeneration(device, &generation);
        (*device)->GetRemoteNodeID(device, generation, &nodeID);

        std::cout << "FW410 operational unit:\n";
        std::cout << "    generation: " << generation << '\n';
        std::cout << "    remote node: 0x" << std::hex << nodeID
                  << std::dec << '\n';

        runProbe(device, generation, nodeID, raw);

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
