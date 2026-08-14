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
#include <vector>

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

static std::vector<UInt8> selectorStatusCommand(UInt8 subunitId, UInt8 fbId) {
    return {
        0x01, static_cast<UInt8>(0x08 | (subunitId & 0x07)), 0xb8,
        0x80, fbId, 0x10, 0x02, 0xff, 0x01, 0x00, 0x00, 0x00
    };
}

static std::vector<UInt8> featureMuteStatusCommand(UInt8 subunitId,
                                                   UInt8 fbId,
                                                   UInt8 channel) {
    // FUNCTION BLOCK / Feature / CURRENT / Mute. Linux oxfw-spkr.c uses
    // selector 0x01 and one-byte mute data; STATUS uses 0xff as query value.
    return {
        0x01, static_cast<UInt8>(0x08 | (subunitId & 0x07)), 0xb8,
        0x81, fbId, 0x10, 0x02, channel, 0x01, 0x01, 0xff
    };
}

static std::vector<UInt8> featureVolumeStatusCommand(UInt8 subunitId,
                                                     UInt8 fbId,
                                                     UInt8 channel) {
    // FUNCTION BLOCK / Feature / CURRENT / Volume. Selector 0x02, signed
    // 16-bit volume value. STATUS queries with ff ff.
    return {
        0x01, static_cast<UInt8>(0x08 | (subunitId & 0x07)), 0xb8,
        0x81, fbId, 0x10, 0x02, channel, 0x02, 0x02, 0xff, 0xff
    };
}

static bool transaction(IOFireWireLibDeviceRef device,
                        UInt32 generation,
                        UInt16 remoteNodeID,
                        ResponseContext& ctx,
                        const std::vector<UInt8>& command,
                        bool raw) {
    resetResponse(ctx);
    UInt32 size = static_cast<UInt32>(command.size());
    const IOReturn kr = writeFcp(device, generation, remoteNodeID,
                                 command.data(), size);
    if (kr != kIOReturnSuccess) return false;

    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
    while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    if (!ctx.received) return false;

    if (raw) {
        std::cout << "        command:  ";
        printBytes(command.data(), static_cast<UInt32>(command.size()));
        std::cout << "\n        response: ";
        printBytes(ctx.bytes.data(), ctx.length);
        std::cout << '\n';
    }
    return true;
}

static bool stable(const ResponseContext& ctx) {
    return ctx.length > 0 && ctx.bytes[0] == 0x0c;
}

static void probeSelectors(IOFireWireLibDeviceRef device,
                           UInt32 generation,
                           UInt16 remoteNodeID,
                           ResponseContext& ctx,
                           bool raw) {
    std::cout << "    Audio subunit 0 selector function blocks (read-only):\n";
    unsigned implemented = 0;
    unsigned responded = 0;
    for (unsigned fb = 0; fb <= kMaxFunctionBlockId; ++fb) {
        const auto command = selectorStatusCommand(0, static_cast<UInt8>(fb));
        if (!transaction(device, generation, remoteNodeID, ctx, command, raw))
            continue;
        ++responded;
        if (ctx.length < 9 || !stable(ctx)) continue;
        ++implemented;
        std::cout << "        selector fb " << fb
                  << ": current input plug=" << static_cast<unsigned>(ctx.bytes[7])
                  << " response=" << responseName(ctx.bytes[0]) << '\n';
    }
    std::cout << "    selector responses: " << responded
              << ", implemented: " << implemented << '\n';
}

static void probeFeatures(IOFireWireLibDeviceRef device,
                          UInt32 generation,
                          UInt16 remoteNodeID,
                          ResponseContext& ctx,
                          bool raw) {
    std::cout << "    Audio subunit 0 feature function blocks (read-only):\n";
    unsigned implementedBlocks = 0;

    for (unsigned fb = 0; fb <= kMaxFunctionBlockId; ++fb) {
        bool blockPrinted = false;

        const auto mute = featureMuteStatusCommand(0, static_cast<UInt8>(fb), 0);
        if (transaction(device, generation, remoteNodeID, ctx, mute, raw) &&
            stable(ctx) && ctx.length >= 11) {
            ++implementedBlocks;
            blockPrinted = true;
            const UInt8 v = ctx.bytes[10];
            std::cout << "        feature fb " << fb
                      << ": master mute raw=0x" << std::hex
                      << static_cast<unsigned>(v) << std::dec;
            if (v == 0x70) std::cout << " (muted)";
            else if (v == 0x60) std::cout << " (unmuted)";
            std::cout << '\n';
        }

        const auto vol = featureVolumeStatusCommand(0, static_cast<UInt8>(fb), 0);
        if (transaction(device, generation, remoteNodeID, ctx, vol, raw) &&
            stable(ctx) && ctx.length >= 12) {
            if (!blockPrinted) ++implementedBlocks;
            blockPrinted = true;
            const std::int16_t value = static_cast<std::int16_t>(
                (static_cast<std::uint16_t>(ctx.bytes[10]) << 8) | ctx.bytes[11]);
            std::cout << "        feature fb " << fb
                      << ": master volume raw=" << value
                      << " (0x" << std::hex
                      << static_cast<unsigned>(static_cast<std::uint16_t>(value))
                      << std::dec << ")\n";

            for (UInt8 ch = 1; ch <= 2; ++ch) {
                const auto chVol = featureVolumeStatusCommand(
                    0, static_cast<UInt8>(fb), ch);
                if (transaction(device, generation, remoteNodeID, ctx, chVol, raw) &&
                    stable(ctx) && ctx.length >= 12) {
                    const std::int16_t chValue = static_cast<std::int16_t>(
                        (static_cast<std::uint16_t>(ctx.bytes[10]) << 8) |
                        ctx.bytes[11]);
                    std::cout << "            channel " << static_cast<unsigned>(ch)
                              << " volume raw=" << chValue << '\n';
                }
            }
        }
    }

    std::cout << "    feature blocks with mute/volume controls: "
              << implementedBlocks << '\n';
}

static bool runProbe(IOFireWireLibDeviceRef device,
                     UInt32 generation,
                     UInt16 remoteNodeID,
                     bool raw) {
    ResponseContext ctx;
    ctx.expectedNode = remoteNodeID;

    if ((*device)->Open(device) != kIOReturnSuccess) return false;
    if ((*device)->AddCallbackDispatcherToRunLoop(
            device, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
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

    probeSelectors(device, generation, remoteNodeID, ctx, raw);
    probeFeatures(device, generation, remoteNodeID, ctx, raw);
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

    std::cout << "macfw mixerprobe — read-only FW410 AV/C mixer probe\n\n";
    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;

    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                     matching, &iterator) != KERN_SUCCESS)
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
        const kern_return_t kr = IOCreatePlugInInterfaceForService(
            service, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID,
            &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) {
            IOObjectRelease(service);
            continue;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
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
