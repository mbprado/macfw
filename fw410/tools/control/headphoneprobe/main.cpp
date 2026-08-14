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
constexpr UInt8 kHeadphoneSelector = 7;

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
    const bool ok = CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringCompare(static_cast<CFStringRef>(value), CFSTR("FW 410"), 0) == kCFCompareEqualTo;
    CFRelease(value);
    return ok;
}

static UInt32 fcpWriteHandler(IOFireWireLibPseudoAddressSpaceRef space,
                              FWClientCommandID commandID,
                              UInt32 packetLen, void* packet,
                              UInt16 srcNodeID, UInt32, UInt32, void* refCon) {
    auto* ctx = static_cast<ResponseContext*>(refCon);
    if (ctx && packet && srcNodeID == ctx->expectedNode) {
        const UInt32 n = std::min<UInt32>(packetLen, ctx->bytes.size());
        std::memcpy(ctx->bytes.data(), packet, n);
        ctx->length = n;
        ctx->received = true;
    }
    (*space)->ClientCommandIsComplete(space, commandID, kIOReturnSuccess);
    return kIOReturnSuccess;
}

static void reset(ResponseContext& ctx) {
    ctx.received = false;
    ctx.length = 0;
    ctx.bytes.fill(0);
}

static bool transact(IOFireWireLibDeviceRef device, UInt32 generation,
                     UInt16 node, ResponseContext& ctx,
                     const std::vector<UInt8>& command) {
    reset(ctx);
    FWAddress addr{};
    addr.nodeID = node;
    addr.addressHi = kAddressHi;
    addr.addressLo = kFcpCommandLo;
    UInt32 size = static_cast<UInt32>(command.size());
    if ((*device)->Write(device, 0, &addr, command.data(), &size, true, generation) != kIOReturnSuccess)
        return false;
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
    while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    return ctx.received;
}

static std::vector<UInt8> selectorStatus(UInt8 fb) {
    return {0x01,0x08,0xb8,0x80,fb,0x10,0x02,0xff,0x01,0x00,0x00,0x00};
}

static std::vector<UInt8> selectorControl(UInt8 fb, UInt8 input) {
    return {0x00,0x08,0xb8,0x80,fb,0x10,0x02,input,0x01,0x00,0x00,0x00};
}

static bool readSelector(IOFireWireLibDeviceRef device, UInt32 generation,
                         UInt16 node, ResponseContext& ctx, UInt8& value) {
    if (!transact(device, generation, node, ctx, selectorStatus(kHeadphoneSelector))) return false;
    if (ctx.length < 9 || ctx.bytes[0] != 0x0c) return false;
    value = ctx.bytes[7];
    return true;
}

static bool writeSelector(IOFireWireLibDeviceRef device, UInt32 generation,
                          UInt16 node, ResponseContext& ctx, UInt8 value) {
    if (!transact(device, generation, node, ctx, selectorControl(kHeadphoneSelector, value))) return false;
    return ctx.length > 0 && (ctx.bytes[0] == 0x09 || ctx.bytes[0] == 0x0c);
}

static bool run(IOFireWireLibDeviceRef device, UInt32 generation, UInt16 node,
                bool execute, int requested, double holdSeconds) {
    ResponseContext ctx;
    ctx.expectedNode = node;
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

    UInt8 original = 0xff;
    bool ok = readSelector(device, generation, node, ctx, original);
    if (!ok || original > 1) {
        std::cout << "selector FB7 read failed or unexpected value\n";
        goto cleanup;
    }

    {
        const UInt8 target = requested >= 0 ? static_cast<UInt8>(requested) : static_cast<UInt8>(original ^ 1u);
        std::cout << "headphone routing candidate: selector FB7\n"
                  << "    current input: " << static_cast<unsigned>(original)
                  << (original == 0 ? " (MAIN/processing candidate)" : " (AUX/common candidate)") << '\n'
                  << "    test input:    " << static_cast<unsigned>(target)
                  << (target == 0 ? " (MAIN/processing candidate)" : " (AUX/common candidate)") << '\n';

        if (!execute) {
            std::cout << "status: dry run; no CONTROL write performed\n"
                      << "to execute: ./headphoneprobe --execute [--select 0|1] [--seconds N]\n";
            ok = true;
            goto cleanup;
        }

        if (target == original) {
            std::cout << "target equals current value; nothing to change\n";
            ok = true;
            goto cleanup;
        }

        std::cout << "switching selector FB7 to input " << static_cast<unsigned>(target)
                  << " for " << holdSeconds << " s...\n";
        if (!writeSelector(device, generation, node, ctx, target)) {
            std::cout << "CONTROL write rejected/failed\n";
            ok = false;
            goto cleanup;
        }

        UInt8 verify = 0xff;
        if (!readSelector(device, generation, node, ctx, verify) || verify != target) {
            std::cout << "verification after switch failed; restoring original\n";
        } else {
            std::cout << "test route active; listen to both headphone jacks now\n";
            const CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + holdSeconds;
            while (CFAbsoluteTimeGetCurrent() < end)
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
        }

        std::cout << "restoring selector FB7 to input " << static_cast<unsigned>(original) << "...\n";
        if (!writeSelector(device, generation, node, ctx, original)) {
            std::cout << "WARNING: restore CONTROL write failed\n";
            ok = false;
            goto cleanup;
        }
        UInt8 restored = 0xff;
        ok = readSelector(device, generation, node, ctx, restored) && restored == original;
        std::cout << "restore: " << (ok ? "PASS" : "FAILED") << '\n';
    }

cleanup:
    (*responseSpace)->TurnOffNotification(responseSpace);
    (*responseSpace)->Release(responseSpace);
    (*device)->RemoveCallbackDispatcherFromRunLoop(device);
    (*device)->Close(device);
    return ok;
}
}

int main(int argc, char** argv) {
    bool execute = false;
    int requested = -1;
    double seconds = 8.0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else if (arg == "--select" && i + 1 < argc) {
            requested = std::stoi(argv[++i]);
            if (requested < 0 || requested > 1) { std::cerr << "--select must be 0 or 1\n"; return 64; }
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::stod(argv[++i]);
            if (seconds <= 0.0 || seconds > 60.0) { std::cerr << "--seconds must be >0 and <=60\n"; return 64; }
        } else {
            std::cerr << "usage: ./headphoneprobe [--execute] [--select 0|1] [--seconds N]\n";
            return 64;
        }
    }

    std::cout << "macfw headphoneprobe — guarded FW410 headphone selector A/B test\n\n";
    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS) return 1;

    bool found = false;
    int result = 1;
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
            CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID), reinterpret_cast<LPVOID*>(&device));
        if (hr != 0 || !device) {
            IODestroyPlugInInterface(plugin); IOObjectRelease(service); continue;
        }
        UInt32 generation = 0; UInt16 node = 0;
        (*device)->GetBusGeneration(device, &generation);
        (*device)->GetRemoteNodeID(device, generation, &node);
        std::cout << "FW410 operational unit:\n"
                  << "    generation: " << generation << '\n'
                  << "    remote node: 0x" << std::hex << node << std::dec << '\n';
        result = run(device, generation, node, execute, requested, seconds) ? 0 : 1;
        (*device)->Release(device);
        IODestroyPlugInInterface(plugin);
        IOObjectRelease(service);
        break;
    }
    IOObjectRelease(iterator);
    if (!found) std::cout << "No operational FW 410 unit found.\n";
    return result;
}
