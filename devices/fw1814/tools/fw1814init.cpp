#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr const char* kProduct = "FW 1814";
constexpr std::uint64_t kVendor = 0x00000d6c;
constexpr std::uint64_t kSpecifier = 0x0000a02d;
constexpr std::uint64_t kUnitSw = 0x00014001;
constexpr UInt16 kAddressHi = 0xffff;
constexpr UInt32 kInfoLo = 0xc8020000;
constexpr UInt32 kFcpCommandLo = 0xf0000b00;
constexpr UInt32 kFcpResponseLo = 0xf0000d00;
constexpr UInt32 kFcpResponseSize = 0x200;
constexpr double kTimeoutSeconds = 1.0;

struct ResponseContext {
    UInt16 expectedNode = 0;
    bool received = false;
    UInt32 length = 0;
    std::array<UInt8, kFcpResponseSize> bytes{};
};

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

UInt32 le32(const UInt8* p) {
    return static_cast<UInt32>(p[0]) |
           (static_cast<UInt32>(p[1]) << 8) |
           (static_cast<UInt32>(p[2]) << 16) |
           (static_cast<UInt32>(p[3]) << 24);
}

std::string asciiField(const UInt8* p, size_t len) {
    std::string out;
    for (size_t i = 0; i < len && p[i]; ++i)
        out.push_back(static_cast<char>(p[i]));
    return out;
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

int sfcForRate(unsigned rate) {
    static constexpr unsigned table[] = {
        32000, 44100, 48000, 88200, 96000, 176400, 192000
    };
    for (int i = 0; i < 7; ++i)
        if (table[i] == rate) return i;
    return -1;
}

IOReturn readAbsolute(IOFireWireLibDeviceRef device, UInt32 generation,
                      UInt16 node, UInt32 lo, void* buffer, UInt32& size) {
    FWAddress a{};
    a.nodeID = node;
    a.addressHi = kAddressHi;
    a.addressLo = lo;
    return (*device)->Read(device, 0, &a, buffer, &size, true, generation);
}

IOReturn writeFcp(IOFireWireLibDeviceRef device, UInt32 generation,
                  UInt16 node, const void* buffer, UInt32& size) {
    FWAddress a{};
    a.nodeID = node;
    a.addressHi = kAddressHi;
    a.addressLo = kFcpCommandLo;
    return (*device)->Write(device, 0, &a, buffer, &size, true, generation);
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
                 const UInt8* command, UInt32 length, bool raw) {
    ctx.received = false;
    ctx.length = 0;
    ctx.bytes.fill(0);

    if (raw) {
        std::cout << "    command:  ";
        printBytes(command, length);
        std::cout << '\n';
    }

    UInt32 size = length;
    const IOReturn kr = writeFcp(device, generation, node, command, size);
    if (kr != kIOReturnSuccess) {
        std::cout << "    FCP write failed: 0x" << std::hex << kr
                  << std::dec << '\n';
        return false;
    }

    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
    while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);

    if (raw && ctx.received) {
        std::cout << "    response: ";
        printBytes(ctx.bytes.data(), ctx.length);
        std::cout << '\n';
    }
    return ctx.received;
}

bool validControlResponse(UInt8 response) {
    return response == 0x09 || response == 0x0c ||
           response == 0x0d || response == 0x0f;
}

bool readInputRate(IOFireWireLibDeviceRef device, UInt32 generation,
                   UInt16 node, ResponseContext& ctx,
                   unsigned& rate, bool raw) {
    const UInt8 command[8] = {
        0x01, 0xff, 0x19, 0x00, 0x90, 0xff, 0xff, 0xff
    };
    if (!transaction(device, generation, node, ctx, command,
                     static_cast<UInt32>(sizeof(command)), raw))
        return false;
    if (ctx.length < 8 ||
        (ctx.bytes[0] != 0x0c && ctx.bytes[0] != 0x0d) ||
        ctx.bytes[1] != 0xff || ctx.bytes[2] != 0x19 ||
        ctx.bytes[3] != 0x00 || ctx.bytes[4] != 0x90)
        return false;
    rate = rateForSfc(ctx.bytes[5] & 0x07);
    return rate != 0;
}

bool setSignalRate(IOFireWireLibDeviceRef device, UInt32 generation,
                   UInt16 node, ResponseContext& ctx,
                   UInt8 opcode, unsigned rate, bool raw) {
    const int sfc = sfcForRate(rate);
    if (sfc < 0) return false;
    const UInt8 command[8] = {
        0x00, 0xff, opcode, 0x00, 0x90,
        static_cast<UInt8>(sfc), 0xff, 0xff
    };
    if (!transaction(device, generation, node, ctx, command,
                     static_cast<UInt32>(sizeof(command)), raw))
        return false;
    if (ctx.length < 8 || !validControlResponse(ctx.bytes[0]))
        return false;
    return ctx.bytes[1] == 0xff && ctx.bytes[2] == opcode &&
           ctx.bytes[3] == 0x00 && ctx.bytes[4] == 0x90 &&
           ((ctx.bytes[5] & 0x07) == static_cast<UInt8>(sfc));
}

bool setKnownClockBaseline(IOFireWireLibDeviceRef device, UInt32 generation,
                           UInt16 node, ResponseContext& ctx, bool raw) {
    // M-Audio special-firmware vendor-dependent CONTROL used by Linux snd-bebob:
    // clock source 0x03 = Internal, digital in/out 0x00 = S/PDIF, unlocked.
    const UInt8 command[12] = {
        0x00, 0xff, 0x00,
        0x04, 0x00, 0x04,
        0x03, 0x00, 0x00, 0x00,
        0x00, 0x00
    };
    if (!transaction(device, generation, node, ctx, command,
                     static_cast<UInt32>(sizeof(command)), raw))
        return false;
    if (ctx.length < 10 || !validControlResponse(ctx.bytes[0]))
        return false;
    for (unsigned i = 1; i <= 9; ++i)
        if (ctx.bytes[i] != command[i]) return false;
    return true;
}

bool validateInfo(IOFireWireLibDeviceRef device, UInt32 generation,
                  UInt16 node) {
    std::array<UInt8, 0x68> info{};
    UInt32 size = static_cast<UInt32>(info.size());
    if (readAbsolute(device, generation, node, kInfoLo,
                     info.data(), size) != kIOReturnSuccess ||
        size != info.size())
        return false;

    const bool ok =
        asciiField(info.data() + 0x00, 8) == "bridgeCo" &&
        le32(info.data() + 0x08) == 1 &&
        le32(info.data() + 0x0c) == 0 &&
        le32(info.data() + 0x18) == 0x83 &&
        le32(info.data() + 0x1c) == 1 &&
        asciiField(info.data() + 0x20, 8) == "20070713" &&
        le32(info.data() + 0x30) == 0 &&
        le32(info.data() + 0x38) == 0x20080000 &&
        le32(info.data() + 0x3c) == 0x00180000;

    std::cout << "BeBoB operational fingerprint: " << (ok ? "PASS" : "FAIL") << '\n';
    return ok;
}

void usage(const char* argv0) {
    std::cout << "usage: " << argv0
              << " [44100|48000] [--execute] [--raw]\n";
}

} // namespace

int main(int argc, char** argv) {
    unsigned targetRate = 48000;
    bool execute = false;
    bool raw = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "44100") targetRate = 44100;
        else if (arg == "48000") targetRate = 48000;
        else if (arg == "--execute") execute = true;
        else if (arg == "--raw") raw = true;
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 64;
        }
    }

    std::cout << "macfw fw1814init — guarded M-Audio special-firmware initializer\n\n"
              << "planned baseline:\n"
              << "    clock:       Internal (0x03)\n"
              << "    digital in:  S/PDIF\n"
              << "    digital out: S/PDIF\n"
              << "    lock:        off\n"
              << "    sample rate: " << targetRate << " Hz\n\n";

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching,
                                     &iterator) != KERN_SUCCESS)
        return 1;

    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        const std::string product = stringProperty(service, "FireWire Product Name");
        if (product != kProduct) {
            IOObjectRelease(service);
            continue;
        }

        std::uint64_t vendor = 0, spec = 0, sw = 0;
        const bool registryOk = numberProperty(service, "Vendor_ID", vendor) &&
                                numberProperty(service, "Unit_Spec_ID", spec) &&
                                numberProperty(service, "Unit_SW_Version", sw) &&
                                vendor == kVendor && spec == kSpecifier && sw == kUnitSw;
        std::cout << "registry identity: " << (registryOk ? "PASS" : "FAIL") << '\n';
        if (!registryOk) {
            IOObjectRelease(service);
            IOObjectRelease(iterator);
            return 2;
        }

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
        std::cout << "generation: " << generation << '\n'
                  << "remote node: 0x" << std::hex << node << std::dec << '\n';

        const IOReturn openKr = (*device)->Open(device);
        if (openKr != kIOReturnSuccess) {
            std::cout << "Open failed: 0x" << std::hex << openKr << std::dec << '\n';
            (*device)->Release(device);
            IOObjectRelease(iterator);
            return 4;
        }

        if (!validateInfo(device, generation, node)) {
            (*device)->Close(device);
            (*device)->Release(device);
            IOObjectRelease(iterator);
            return 5;
        }

        if (!execute) {
            std::cout << "\nstatus: PASS - dry run only; no CONTROL commands sent\n"
                      << "to execute: " << argv[0] << ' ' << targetRate
                      << " --execute" << (raw ? " --raw" : "") << '\n';
            (*device)->Close(device);
            (*device)->Release(device);
            IOObjectRelease(iterator);
            return 0;
        }

        if ((*device)->AddCallbackDispatcherToRunLoop(
                device, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
            (*device)->Close(device);
            (*device)->Release(device);
            IOObjectRelease(iterator);
            return 6;
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
            return 6;
        }
        (*responseSpace)->SetWriteHandler(responseSpace, responseHandler);
        (*responseSpace)->TurnOnNotification(responseSpace);

        std::cout << "\nsetting known clock/digital baseline...\n";
        const bool clockOk = setKnownClockBaseline(device, generation, node, ctx, raw);
        std::cout << "    result: " << (clockOk ? "PASS" : "FAIL") << '\n';

        bool rateOk = false;
        if (clockOk) {
            std::cout << "setting device OUTPUT signal format to " << targetRate << " Hz...\n";
            const bool outOk = setSignalRate(device, generation, node, ctx,
                                             0x18, targetRate, raw);
            std::cout << "    OUTPUT result: " << (outOk ? "PASS" : "FAIL") << '\n';

            std::cout << "waiting 100 ms before INPUT rate CONTROL (FW1814 quirk)...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            std::cout << "setting device INPUT signal format to " << targetRate << " Hz...\n";
            const bool inOk = setSignalRate(device, generation, node, ctx,
                                            0x19, targetRate, raw);
            std::cout << "    INPUT result: " << (inOk ? "PASS" : "FAIL") << '\n';

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            unsigned actualRate = 0;
            const bool readOk = readInputRate(device, generation, node, ctx,
                                              actualRate, raw);
            std::cout << "authoritative INPUT rate readback: "
                      << (readOk ? std::to_string(actualRate) : "unknown")
                      << " Hz\n";
            rateOk = outOk && inOk && readOk && actualRate == targetRate;
        }

        std::cout << "known S/PDIF stream formation at 44.1/48 kHz:\n"
                  << "    device -> host capture:  10 PCM + 1 MIDI\n"
                  << "    host -> device playback: 6 PCM + 1 MIDI\n";

        (*responseSpace)->TurnOffNotification(responseSpace);
        (*responseSpace)->Release(responseSpace);
        (*device)->RemoveCallbackDispatcherFromRunLoop(device);
        (*device)->Close(device);
        (*device)->Release(device);
        IOObjectRelease(iterator);

        const bool ok = clockOk && rateOk;
        std::cout << "\nstatus: " << (ok ? "PASS" : "FAIL") << '\n';
        return ok ? 0 : 7;
    }

    IOObjectRelease(iterator);
    std::cout << "No operational FW 1814 unit found.\n";
    return 2;
}
