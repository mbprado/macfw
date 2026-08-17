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

struct ResponseContext {
    UInt16 expectedNode = 0;
    bool received = false;
    UInt32 length = 0;
    std::array<UInt8, kFcpResponseSize> bytes{};
};

bool isFw410(io_registry_entry_t service) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(service, CFSTR("FireWire Product Name"),
                                                   kCFAllocatorDefault, 0);
    if (!v) return false;
    const bool ok = CFGetTypeID(v) == CFStringGetTypeID() &&
        CFStringCompare(static_cast<CFStringRef>(v), CFSTR("FW 410"), 0) == kCFCompareEqualTo;
    CFRelease(v);
    return ok;
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

void printBytes(const UInt8* p, UInt32 n) {
    for (UInt32 i = 0; i < n; ++i) {
        if (i) std::cout << ' ';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(p[i]);
    }
    std::cout << std::dec << std::setfill(' ');
}

unsigned rateForSfc(UInt8 sfc) {
    static constexpr unsigned table[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000, 0};
    return sfc < 8 ? table[sfc] : 0;
}

int sfcForRate(unsigned rate) {
    static constexpr unsigned table[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000};
    for (int i = 0; i < 7; ++i) if (table[i] == rate) return i;
    return -1;
}

bool transaction(IOFireWireLibDeviceRef dev, UInt32 generation, UInt16 node,
                 ResponseContext& ctx, const UInt8* cmd, UInt32 len, bool raw) {
    ctx.received = false;
    ctx.length = 0;
    ctx.bytes.fill(0);
    if (raw) {
        std::cout << "        command:  "; printBytes(cmd, len); std::cout << '\n';
    }
    FWAddress a{}; a.nodeID = node; a.addressHi = kAddressHi; a.addressLo = kFcpCommandLo;
    UInt32 size = len;
    if ((*dev)->Write(dev, 0, &a, cmd, &size, true, generation) != kIOReturnSuccess) return false;
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
    while (!ctx.received && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    if (raw && ctx.received) {
        std::cout << "        response: "; printBytes(ctx.bytes.data(), ctx.length); std::cout << '\n';
    }
    return ctx.received;
}

bool readRate(IOFireWireLibDeviceRef dev, UInt32 gen, UInt16 node,
              ResponseContext& ctx, UInt8 opcode, unsigned& rate, bool raw) {
    const UInt8 cmd[8] = {0x01, 0xff, opcode, 0x00, 0x90, 0xff, 0xff, 0xff};
    if (!transaction(dev, gen, node, ctx, cmd, sizeof(cmd), raw)) return false;
    if (ctx.length < 8 || (ctx.bytes[0] != 0x0c && ctx.bytes[0] != 0x0d) ||
        ctx.bytes[1] != 0xff || ctx.bytes[2] != opcode || ctx.bytes[3] != 0x00 || ctx.bytes[4] != 0x90)
        return false;
    rate = rateForSfc(ctx.bytes[5] & 0x07);
    return rate != 0;
}

bool setRate(IOFireWireLibDeviceRef dev, UInt32 gen, UInt16 node,
             ResponseContext& ctx, UInt8 opcode, unsigned rate, bool raw) {
    const int sfc = sfcForRate(rate);
    if (sfc < 0) return false;
    const UInt8 cmd[8] = {0x00, 0xff, opcode, 0x00, 0x90,
                          static_cast<UInt8>(sfc), 0xff, 0xff};
    if (!transaction(dev, gen, node, ctx, cmd, sizeof(cmd), raw)) return false;

    // AV/C CONTROL may complete immediately (ACCEPTED/IMPLEMENTED/CHANGED)
    // or return INTERIM (0x0f) while the device performs the transition.
    // Treat INTERIM as a valid deferred acceptance; the caller's STATUS
    // readback after the settling delay is authoritative for completion.
    const UInt8 response = ctx.length ? ctx.bytes[0] : 0;
    const bool accepted = response == 0x09 || response == 0x0c ||
                          response == 0x0d || response == 0x0f;
    return ctx.length >= 8 && accepted &&
           ctx.bytes[1] == 0xff && ctx.bytes[2] == opcode && ctx.bytes[3] == 0x00 &&
           ctx.bytes[4] == 0x90 && ((ctx.bytes[5] & 0x07) == static_cast<UInt8>(sfc));
}

bool run(unsigned targetRate, bool execute, bool keep, bool raw) {
    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return false;
    io_iterator_t it = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &it) != KERN_SUCCESS) return false;

    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        if (!isFw410(service)) { IOObjectRelease(service); continue; }

        IOCFPlugInInterface** plugin = nullptr; SInt32 score = 0;
        kern_return_t kr = IOCreatePlugInInterfaceForService(service, kIOFireWireLibTypeID,
            kIOCFPlugInInterfaceID, &plugin, &score);
        IOObjectRelease(service);
        if (kr != KERN_SUCCESS || !plugin) break;

        IOFireWireLibDeviceRef dev = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(plugin,
            CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID), reinterpret_cast<LPVOID*>(&dev));
        IODestroyPlugInInterface(plugin);
        if (hr != 0 || !dev) break;

        UInt32 gen = 0; UInt16 node = 0;
        (*dev)->GetBusGeneration(dev, &gen); (*dev)->GetRemoteNodeID(dev, gen, &node);
        std::cout << "FW410 operational unit:\n    generation: " << gen
                  << "\n    remote node: 0x" << std::hex << node << std::dec << '\n';
        if ((*dev)->Open(dev) != kIOReturnSuccess) { (*dev)->Release(dev); break; }
        (*dev)->AddCallbackDispatcherToRunLoop(dev, CFRunLoopGetCurrent());

        ResponseContext ctx; ctx.expectedNode = node;
        auto responseSpace = (*dev)->CreateInitialUnitsPseudoAddressSpace(dev, kFcpResponseLo,
            kFcpResponseSize, &ctx, 1024, nullptr,
            kFWAddressSpaceNoReadAccess | kFWAddressSpaceShareIfExists,
            CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
        if (!responseSpace) { (*dev)->Close(dev); (*dev)->Release(dev); break; }
        (*responseSpace)->SetWriteHandler(responseSpace, responseHandler);
        (*responseSpace)->TurnOnNotification(responseSpace);

        unsigned outRate = 0, inRate = 0;
        const bool outOk = readRate(dev, gen, node, ctx, 0x18, outRate, raw);
        const bool inOk  = readRate(dev, gen, node, ctx, 0x19, inRate, raw);
        std::cout << "current sample rate:\n"
                  << "    device OUTPUT / host capture:  " << (outOk ? std::to_string(outRate) : "unknown") << " Hz\n"
                  << "    device INPUT / host playback: " << (inOk ? std::to_string(inRate) : "unknown") << " Hz\n";

        bool ok = outOk && inOk;
        if (targetRate) {
            std::cout << "requested rate: " << targetRate << " Hz\n";
            if (!execute) {
                std::cout << "status: PASS - dry run only; no CONTROL command sent\n"
                          << "to execute: ./rateprobe --execute " << targetRate << " [--keep] [--raw]\n";
            } else {
                const unsigned restoreOut = outRate, restoreIn = inRate;
                std::cout << "setting OUTPUT plug 0...\n";
                const bool controlOut = setRate(dev, gen, node, ctx, 0x18, targetRate, raw);
                std::cout << "setting INPUT plug 0...\n";
                const bool controlIn = setRate(dev, gen, node, ctx, 0x19, targetRate, raw);
                if ((!controlOut || !controlIn) && raw)
                    std::cout << "CONTROL response was non-final; relying on authoritative STATUS readback\n";
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.35, false);
                unsigned verifyOut = 0, verifyIn = 0;
                const bool verified = readRate(dev, gen, node, ctx, 0x18, verifyOut, raw) &&
                                      readRate(dev, gen, node, ctx, 0x19, verifyIn, raw) &&
                                      verifyOut == targetRate && verifyIn == targetRate;
                std::cout << "readback: out=" << verifyOut << " Hz, in=" << verifyIn
                          << " Hz -> " << (verified ? "PASS" : "FAIL") << '\n';
                // STATUS readback is authoritative. Some FW410 CONTROL transactions can
                // return a response shape our strict parser does not accept even though
                // the requested clock transition has completed successfully.
                ok = ok && verified;
                if (!keep) {
                    std::cout << "restoring original rates...\n";
                    const bool r1 = setRate(dev, gen, node, ctx, 0x18, restoreOut, raw);
                    const bool r2 = setRate(dev, gen, node, ctx, 0x19, restoreIn, raw);
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.35, false);
                    unsigned ro = 0, ri = 0;
                    const bool rv = readRate(dev, gen, node, ctx, 0x18, ro, raw) &&
                                    readRate(dev, gen, node, ctx, 0x19, ri, raw) &&
                                    ro == restoreOut && ri == restoreIn;
                    std::cout << "restore readback: out=" << ro << " Hz, in=" << ri
                              << " Hz -> " << (rv ? "PASS" : "FAIL") << '\n';
                    (void)r1; (void)r2;
                    ok = ok && rv;
                } else {
                    std::cout << "rate left at " << targetRate << " Hz (--keep)\n";
                }
            }
        }

        (*responseSpace)->TurnOffNotification(responseSpace);
        (*responseSpace)->Release(responseSpace);
        (*dev)->RemoveCallbackDispatcherFromRunLoop(dev);
        (*dev)->Close(dev); (*dev)->Release(dev); IOObjectRelease(it);
        return ok;
    }
    IOObjectRelease(it);
    std::cout << "No operational FW 410 unit found.\n";
    return false;
}
}

int main(int argc, char** argv) {
    bool execute = false, keep = false, raw = false;
    unsigned rate = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--execute") execute = true;
        else if (a == "--keep") keep = true;
        else if (a == "--raw") raw = true;
        else if (a == "44100") rate = 44100;
        else if (a == "48000") rate = 48000;
        else { std::cerr << "usage: ./rateprobe [44100|48000] [--execute] [--keep] [--raw]\n"; return 64; }
    }
    if (keep && !execute) { std::cerr << "--keep requires --execute\n"; return 64; }
    std::cout << "macfw rateprobe — guarded FW410 AV/C sample-rate control probe\n\n";
    return run(rate, execute, keep, raw) ? 0 : 1;
}
