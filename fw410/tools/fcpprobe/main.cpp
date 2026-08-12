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
constexpr UInt32 kFcpResponseInitialUnitsLo = 0xf0000d00;
constexpr UInt32 kFcpResponseSize = 0x200;
constexpr double kTimeoutSeconds = 1.0;

struct ResponseContext {
    UInt16 expectedNode = 0;
    bool received = false;
    UInt16 sourceNode = 0;
    UInt32 length = 0;
    std::array<UInt8, kFcpResponseSize> bytes{};
};

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
        CFNumberGetValue(static_cast<CFNumberRef>(value),
                         kCFNumberLongLongType, &number);
        std::cout << "    " << key << ": 0x" << std::hex << number
                  << std::dec << '\n';
    }
    CFRelease(value);
}

static bool isOperationalFw410(io_registry_entry_t service) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, CFSTR("FireWire Product Name"), kCFAllocatorDefault, 0);
    if (!value) return false;

    bool result = false;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        result = CFStringCompare(static_cast<CFStringRef>(value),
                                 CFSTR("FW 410"), 0) == kCFCompareEqualTo;
    }
    CFRelease(value);
    return result;
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
    return (*device)->Write(device, 0, &address, buffer, &size,
                            true, generation);
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

    (*addressSpace)->ClientCommandIsComplete(addressSpace, commandID,
                                             kIOReturnSuccess);
    return kIOReturnSuccess;
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

static unsigned rateForSfc(UInt8 sfc) {
    static constexpr unsigned rates[] = {
        32000, 44100, 48000, 88200,
        96000, 176400, 192000, 0
    };
    return sfc < sizeof(rates) / sizeof(rates[0]) ? rates[sfc] : 0;
}

static bool runOutputSignalFormatProbe(IOFireWireLibDeviceRef device,
                                       UInt32 generation,
                                       UInt16 remoteNodeID) {
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
        std::cout << "    callback dispatcher: failed (0x" << std::hex
                  << dispatcherResult << std::dec << ")\n";
        (*device)->Close(device);
        return false;
    }

    IOFireWireLibPseudoAddressSpaceRef responseSpace =
        (*device)->CreateInitialUnitsPseudoAddressSpace(
            device,
            kFcpResponseInitialUnitsLo,
            kFcpResponseSize,
            &ctx,
            1024,
            nullptr,
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
    std::cout << "    local FCP response window: node=0x" << std::hex
              << localAddress.nodeID << " hi=0x" << localAddress.addressHi
              << " lo=0x" << localAddress.addressLo << std::dec << '\n';

    if (localAddress.addressHi != 0xffff ||
        localAddress.addressLo != kFcpResponseInitialUnitsLo) {
        std::cout << "    response window mismatch: expected hi=0xffff lo=0xf0000d00\n";
    }

    UInt8 command[8] = {0x01, 0xff, 0x18, 0x00,
                        0x90, 0xff, 0xff, 0xff};
    UInt32 commandSize = sizeof(command);

    std::cout << "    FCP command address: 0xfffff0000b00\n"
              << "    AV/C command:        ";
    printBytes(command, commandSize);
    std::cout << '\n';

    const IOReturn writeResult = writeAbsolute(device, generation, remoteNodeID,
                                               command, commandSize);
    if (writeResult != kIOReturnSuccess) {
        std::cout << "    command write: failed (0x" << std::hex << writeResult
                  << std::dec << ")\n";
    } else {
        std::cout << "    command write: success (" << commandSize << " bytes)\n";
        const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
        while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
        }

        if (!ctx.received) {
            std::cout << "    response: timeout\n";
        } else {
            std::cout << "    response source: 0x" << std::hex << ctx.sourceNode
                      << std::dec << '\n';
            std::cout << "    response (" << ctx.length << " bytes): ";
            printBytes(ctx.bytes.data(), ctx.length);
            std::cout << '\n';

            if (ctx.length >= 1) {
                std::cout << "    AV/C response: 0x" << std::hex
                          << static_cast<unsigned>(ctx.bytes[0]) << std::dec
                          << " (" << responseName(ctx.bytes[0]) << ")\n";
            }

            if (ctx.length >= 8 &&
                ctx.bytes[1] == 0xff &&
                ctx.bytes[2] == 0x18 &&
                ctx.bytes[3] == 0x00 &&
                ctx.bytes[4] == 0x90) {
                const UInt8 sfc = ctx.bytes[5] & 0x07;
                const unsigned rate = rateForSfc(sfc);
                std::cout << "    AM824 SFC: 0x" << std::hex
                          << static_cast<unsigned>(sfc) << std::dec;
                if (rate) std::cout << " -> " << rate << " Hz";
                std::cout << '\n';
            } else if (ctx.length >= 8) {
                std::cout << "    signal-format response shape: unexpected\n";
            }
        }
    }

    (*responseSpace)->TurnOffNotification(responseSpace);
    (*responseSpace)->Release(responseSpace);
    (*device)->RemoveCallbackDispatcherFromRunLoop(device);
    (*device)->Close(device);
    return writeResult == kIOReturnSuccess && ctx.received;
}

} // namespace

int main() {
    std::cout << "macfw fcpprobe — raw FCP / AV/C status probe\n\n";

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) {
        std::cerr << "Unable to create IOFireWireUnit matching dictionary\n";
        return 1;
    }

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
        printProperty(service, "Unit_Spec_ID");
        printProperty(service, "Unit_SW_Version");

        IOCFPlugInInterface **plugin = nullptr;
        SInt32 score = 0;
        kr = IOCreatePlugInInterfaceForService(service,
                                               kIOFireWireLibTypeID,
                                               kIOCFPlugInInterfaceID,
                                               &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) {
            std::cout << "    IOFireWireLib plugin unavailable (0x"
                      << std::hex << kr << std::dec << ")\n";
            IOObjectRelease(service);
            continue;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin,
            CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID *>(&device));
        if (hr != 0 || !device) {
            std::cout << "    IOFireWireDeviceInterface unavailable (0x"
                      << std::hex << static_cast<unsigned long>(hr)
                      << std::dec << ")\n";
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

        std::cout << "    interface version: " << (*device)->version << '\n';
        if (genResult == kIOReturnSuccess && nodeResult == kIOReturnSuccess) {
            std::cout << "    generation:        " << generation << '\n'
                      << "    remote node:       0x" << std::hex << nodeID
                      << std::dec << '\n';
            runOutputSignalFormatProbe(device, generation, nodeID);
        } else {
            std::cout << "    unable to obtain generation/node ID\n";
        }

        (*device)->Release(device);
        IODestroyPlugInInterface(plugin);
        IOObjectRelease(service);
        std::cout << '\n';
    }

    IOObjectRelease(iterator);
    if (!found) {
        std::cout << "No operational FW 410 unit found. Run fwboot first if the device is still in bootloader mode.\n";
        return 2;
    }
    return 0;
}
