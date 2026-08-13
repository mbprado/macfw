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
constexpr double kTimeoutSeconds = 1.0;

struct ResponseContext {
    UInt16 expectedNode = 0;
    bool received = false;
    UInt16 sourceNode = 0;
    UInt32 length = 0;
    std::array<UInt8, kFcpResponseSize> bytes{};
};

struct ChannelPosition {
    unsigned section = 0;
    unsigned streamPosition = 0;
    unsigned sectionLocation = 0;
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
                               sizeof(buffer), kCFStringEncodingUTF8)) {
            std::cout << "    " << key << ": " << buffer << '\n';
        }
    } else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        long long number = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &number);
        std::cout << "    " << key << ": 0x" << std::hex << number << std::dec << '\n';
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

static const char *responseName(UInt8 response) {
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

static const char *plugTypeName(UInt8 type) {
    switch (type) {
        case 0x00: return "isochronous";
        case 0x01: return "asynchronous";
        case 0x02: return "MIDI";
        case 0x03: return "sync";
        case 0x04: return "analog";
        case 0x05: return "digital";
        case 0x06: return "additional";
        default: return "unknown";
    }
}

static const char *sectionTypeName(UInt8 type) {
    switch (type) {
        case 0x01: return "headphone";
        case 0x02: return "microphone";
        case 0x03: return "line";
        case 0x04: return "S/PDIF";
        case 0x05: return "ADAT";
        case 0x06: return "TDIF";
        case 0x07: return "MADI";
        case 0x08: return "analog/undefined";
        case 0x09: return "digital/undefined";
        case 0x0a: return "MIDI conformant";
        case 0xff: return "no type";
        default: return "unknown";
    }
}

static IOReturn writeAbsolute(IOFireWireLibDeviceRef device,
                              UInt32 generation,
                              UInt16 remoteNodeID,
                              const void *buffer,
                              UInt32 &size) {
    FWAddress address = {};
    address.nodeID = remoteNodeID;
    address.addressHi = kAddressHi;
    address.addressLo = kFcpCommandLo;
    return (*device)->Write(device, 0, &address, buffer, &size, true, generation);
}

static UInt32 fcpWriteHandler(IOFireWireLibPseudoAddressSpaceRef addressSpace,
                              FWClientCommandID commandID,
                              UInt32 packetLen,
                              void *packet,
                              UInt16 srcNodeID,
                              UInt32,
                              UInt32,
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

static bool transaction(IOFireWireLibDeviceRef device,
                        UInt32 generation,
                        UInt16 remoteNodeID,
                        ResponseContext &ctx,
                        const std::array<UInt8, 12> &command,
                        const char *label) {
    resetResponse(ctx);
    UInt32 size = static_cast<UInt32>(command.size());
    std::cout << "        " << label << " command: ";
    printBytes(command.data(), size);
    std::cout << '\n';

    const IOReturn kr = writeAbsolute(device, generation, remoteNodeID,
                                      command.data(), size);
    if (kr != kIOReturnSuccess) {
        std::cout << "        write: failed (0x" << std::hex << kr << std::dec << ")\n";
        return false;
    }

    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
    while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }
    if (!ctx.received) {
        std::cout << "        response: timeout\n";
        return false;
    }

    std::cout << "        response (" << ctx.length << " bytes): ";
    printBytes(ctx.bytes.data(), ctx.length);
    std::cout << '\n';
    if (ctx.length) {
        std::cout << "        AV/C response: 0x" << std::hex
                  << static_cast<unsigned>(ctx.bytes[0]) << std::dec
                  << " (" << responseName(ctx.bytes[0]) << ")\n";
    }
    return ctx.length > 0 && ctx.bytes[0] == 0x0c;
}

static std::array<UInt8, 12> bridgecoCommand(UInt8 direction,
                                             UInt8 infoType,
                                             UInt8 operand10 = 0) {
    // BridgeCo unit isochronous plug 0 address:
    // unit=ff, dir={00 in,01 out}, mode=00 unit, unit-type=00 isoc,
    // plug-id=00, reserved=ff.
    return {
        0x01, 0xff, 0x02, 0xc0,
        direction, 0x00, 0x00, 0x00, 0xff,
        infoType, operand10, 0x00
    };
}

static bool probeDirection(IOFireWireLibDeviceRef device,
                           UInt32 generation,
                           UInt16 remoteNodeID,
                           ResponseContext &ctx,
                           UInt8 direction,
                           const char *name) {
    std::cout << "    BridgeCo " << name << " unit isochronous plug 0:\n";

    bool ok = true;

    const auto typeCmd = bridgecoCommand(direction, 0x00);
    if (transaction(device, generation, remoteNodeID, ctx, typeCmd, "plug type") &&
        ctx.length >= 11) {
        const UInt8 type = ctx.bytes[10];
        std::cout << "        plug type: 0x" << std::hex
                  << static_cast<unsigned>(type) << std::dec
                  << " (" << plugTypeName(type) << ")\n";
    } else {
        ok = false;
    }

    const auto countCmd = bridgecoCommand(direction, 0x02);
    if (transaction(device, generation, remoteNodeID, ctx, countCmd, "channel count") &&
        ctx.length >= 11) {
        std::cout << "        reported channel count: "
                  << static_cast<unsigned>(ctx.bytes[10]) << '\n';
    } else {
        ok = false;
    }

    const auto posCmd = bridgecoCommand(direction, 0x03);
    unsigned sectionCount = 0;
    std::vector<unsigned> sectionChannels;
    if (transaction(device, generation, remoteNodeID, ctx, posCmd, "channel positions") &&
        ctx.length >= 11) {
        size_t pos = 10;
        sectionCount = ctx.bytes[pos++];
        std::cout << "        sections: " << sectionCount << '\n';
        bool parseOk = true;
        for (unsigned sec = 0; sec < sectionCount; ++sec) {
            if (pos >= ctx.length) { parseOk = false; break; }
            const unsigned channels = ctx.bytes[pos++];
            sectionChannels.push_back(channels);
            std::cout << "        section " << sec << " channels: " << channels << '\n';
            for (unsigned ch = 0; ch < channels; ++ch) {
                if (pos + 1 >= ctx.length) { parseOk = false; break; }
                const unsigned streamPosition = ctx.bytes[pos++];
                const unsigned sectionLocation = ctx.bytes[pos++];
                std::cout << "            ch " << ch
                          << ": stream-position=" << streamPosition
                          << " section-location=" << sectionLocation << '\n';
            }
            if (!parseOk) break;
        }
        if (!parseOk) {
            std::cout << "        channel-position payload: truncated/unexpected\n";
            ok = false;
        }
    } else {
        ok = false;
    }

    for (unsigned sec = 0; sec < sectionCount; ++sec) {
        const auto sectionCmd = bridgecoCommand(direction, 0x07,
                                                static_cast<UInt8>(sec + 1));
        const std::string label = "section " + std::to_string(sec) + " type";
        if (transaction(device, generation, remoteNodeID, ctx, sectionCmd, label.c_str()) &&
            ctx.length >= 12) {
            const UInt8 type = ctx.bytes[11];
            std::cout << "        section " << sec << " type: 0x"
                      << std::hex << static_cast<unsigned>(type) << std::dec
                      << " (" << sectionTypeName(type) << ")";
            if (sec < sectionChannels.size())
                std::cout << ", channels=" << sectionChannels[sec];
            std::cout << '\n';
        } else {
            ok = false;
        }
    }

    return ok;
}

static bool runProbe(IOFireWireLibDeviceRef device,
                     UInt32 generation,
                     UInt16 remoteNodeID) {
    ResponseContext ctx;
    ctx.expectedNode = remoteNodeID;

    const IOReturn openResult = (*device)->Open(device);
    if (openResult != kIOReturnSuccess) {
        std::cout << "    open: failed (0x" << std::hex << openResult << std::dec << ")\n";
        return false;
    }

    const IOReturn dispatcherResult =
        (*device)->AddCallbackDispatcherToRunLoop(device, CFRunLoopGetCurrent());
    if (dispatcherResult != kIOReturnSuccess) {
        std::cout << "    callback dispatcher: failed (0x" << std::hex
                  << dispatcherResult << std::dec << ")\n";
        (*device)->Close(device);
        return false;
    }

    IOFireWireLibPseudoAddressSpaceRef responseSpace =
        (*device)->CreateInitialUnitsPseudoAddressSpace(
            device, kFcpResponseLo, kFcpResponseSize, &ctx, 1024, nullptr,
            kFWAddressSpaceNoReadAccess | kFWAddressSpaceShareIfExists,
            CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
    if (!responseSpace) {
        std::cout << "    FCP response address space: unavailable\n";
        (*device)->RemoveCallbackDispatcherFromRunLoop(device);
        (*device)->Close(device);
        return false;
    }

    (*responseSpace)->SetWriteHandler(responseSpace, fcpWriteHandler);
    if (!(*responseSpace)->TurnOnNotification(responseSpace)) {
        std::cout << "    FCP response notifications: failed\n";
        (*responseSpace)->Release(responseSpace);
        (*device)->RemoveCallbackDispatcherFromRunLoop(device);
        (*device)->Close(device);
        return false;
    }

    FWAddress localAddress = {};
    (*responseSpace)->GetFWAddress(responseSpace, &localAddress);
    std::cout << "    local FCP response window: hi=0x" << std::hex
              << localAddress.addressHi << " lo=0x" << localAddress.addressLo
              << std::dec << '\n';

    const bool outputOk = probeDirection(device, generation, remoteNodeID, ctx,
                                         0x01, "OUTPUT");
    const bool inputOk = probeDirection(device, generation, remoteNodeID, ctx,
                                        0x00, "INPUT");

    (*responseSpace)->TurnOffNotification(responseSpace);
    (*responseSpace)->Release(responseSpace);
    (*device)->RemoveCallbackDispatcherFromRunLoop(device);
    (*device)->Close(device);

    return outputOk && inputOk;
}

} // namespace

int main() {
    std::cout << "macfw streamprobe — BridgeCo/BeBoB stream topology probe\n\n";

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;

    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                    matching, &iterator);
    if (kr != KERN_SUCCESS) {
        std::cerr << "IOServiceGetMatchingServices failed: 0x" << std::hex
                  << kr << std::dec << '\n';
        return 1;
    }

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
                                               kIOCFPlugInInterfaceID,
                                               &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) {
            std::cout << "    IOFireWireLib plugin unavailable\n";
            IOObjectRelease(service);
            continue;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID *>(&device));
        if (hr != 0 || !device) {
            std::cout << "    IOFireWireDeviceInterface unavailable\n";
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            continue;
        }

        UInt32 generation = 0;
        UInt16 nodeID = 0;
        const IOReturn genResult = (*device)->GetBusGeneration(device, &generation);
        const IOReturn nodeResult = genResult == kIOReturnSuccess
            ? (*device)->GetRemoteNodeID(device, generation, &nodeID)
            : genResult;
        if (genResult == kIOReturnSuccess && nodeResult == kIOReturnSuccess) {
            std::cout << "    generation: " << generation << '\n'
                      << "    remote node: 0x" << std::hex << nodeID
                      << std::dec << '\n';
            runProbe(device, generation, nodeID);
        }

        (*device)->Release(device);
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
