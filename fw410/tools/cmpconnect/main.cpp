#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <libkern/OSByteOrder.h>

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

constexpr UInt16 kAddressHi = 0xffff;
constexpr UInt32 kOpcr0Lo = 0xf0000904;
constexpr UInt32 kIpcr0Lo = 0xf0000984;

constexpr uint32_t kPcrOnline = 0x80000000u;
constexpr uint32_t kPcrBroadcast = 0x40000000u;
constexpr uint32_t kPcrP2PMask = 0x3f000000u;
constexpr uint32_t kPcrChannelMask = 0x003f0000u;
constexpr uint32_t kOpcrXSpeedMask = 0x00c00000u;
constexpr uint32_t kOpcrSpeedMask = 0x0000c000u;
constexpr uint32_t kOpcrOverheadMask = 0x00003c00u;

constexpr UInt32 kCapturePayload48k = 128;
constexpr UInt32 kPlaybackPayload48k = 272;

struct PortContext {
    const char *name = nullptr;
    bool allocated = false;
    IOFWSpeed speed = kFWSpeed100MBit;
    UInt32 channel = 0;
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

static uint32_t be32(const std::array<UInt8,4>& b) {
    return (static_cast<uint32_t>(b[0]) << 24) |
           (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) |
           static_cast<uint32_t>(b[3]);
}

static bool readReg(IOFireWireLibDeviceRef device, UInt32 generation,
                    UInt16 nodeID, UInt32 lo, uint32_t& value) {
    FWAddress address{};
    address.nodeID = nodeID;
    address.addressHi = kAddressHi;
    address.addressLo = lo;
    std::array<UInt8,4> bytes{};
    UInt32 size = static_cast<UInt32>(bytes.size());
    const IOReturn kr = (*device)->Read(device, 0, &address, bytes.data(), &size,
                                       true, generation);
    if (kr != kIOReturnSuccess || size != 4) {
        std::cout << "    read failed at 0xffff" << std::hex << lo
                  << " (0x" << kr << ")" << std::dec << '\n';
        return false;
    }
    value = be32(bytes);
    return true;
}

static bool compareSwapReg(IOFireWireLibDeviceRef device, UInt32 generation,
                           UInt16 nodeID, UInt32 lo,
                           uint32_t expectedHost, uint32_t newHost) {
    FWAddress address{};
    address.nodeID = nodeID;
    address.addressHi = kAddressHi;
    address.addressLo = lo;
    const UInt32 expectedBus = OSSwapHostToBigInt32(expectedHost);
    const UInt32 newBus = OSSwapHostToBigInt32(newHost);
    const IOReturn kr = (*device)->CompareSwap(device, 0, &address,
                                              expectedBus, newBus,
                                              true, generation);
    if (kr != kIOReturnSuccess) {
        std::cout << "    compare-swap failed at 0xffff" << std::hex << lo
                  << " (0x" << kr << ")" << std::dec << '\n';
        return false;
    }
    return true;
}

static bool pcrReady(uint32_t value) {
    const bool online = (value & kPcrOnline) != 0;
    const bool used = (value & (kPcrBroadcast | kPcrP2PMask)) != 0;
    return online && !used;
}

static void printPcr(const char *name, uint32_t v) {
    std::cout << "    " << name << ": 0x" << std::hex << std::setw(8)
              << std::setfill('0') << v << std::dec << std::setfill(' ') << '\n';
    std::cout << "        online: " << ((v & kPcrOnline) ? "yes" : "no")
              << ", p2p=" << ((v >> 24) & 0x3f)
              << ", channel=" << ((v >> 16) & 0x3f) << '\n';
}

static IOReturn remoteGetSupported(IOFireWireLibIsochPortRef,
                                   IOFWSpeed *outMaxSpeed,
                                   UInt64 *outChannels) {
    if (outMaxSpeed) *outMaxSpeed = kFWSpeed400MBit;
    if (outChannels) *outChannels = ~static_cast<UInt64>(0);
    return kIOReturnSuccess;
}

static IOReturn remoteAllocate(IOFireWireLibIsochPortRef interface,
                               IOFWSpeed speed, UInt32 channel) {
    auto *ctx = static_cast<PortContext *>((*interface)->GetRefCon(interface));
    if (ctx) {
        ctx->allocated = true;
        ctx->speed = speed;
        ctx->channel = channel;
    }
    return kIOReturnSuccess;
}

static IOReturn remoteRelease(IOFireWireLibIsochPortRef interface) {
    auto *ctx = static_cast<PortContext *>((*interface)->GetRefCon(interface));
    if (ctx) ctx->allocated = false;
    return kIOReturnSuccess;
}

static IOReturn remoteNoop(IOFireWireLibIsochPortRef) {
    return kIOReturnSuccess;
}

static bool configureRemotePort(IOFireWireLibRemoteIsochPortRef port,
                                PortContext *ctx) {
    if (!port) return false;
    auto basePort = reinterpret_cast<IOFireWireLibIsochPortRef>(port);
    (*basePort)->SetRefCon(basePort, ctx);
    (*port)->SetGetSupportedHandler(port, remoteGetSupported);
    (*port)->SetAllocatePortHandler(port, remoteAllocate);
    (*port)->SetReleasePortHandler(port, remoteRelease);
    (*port)->SetStartHandler(port, remoteNoop);
    (*port)->SetStopHandler(port, remoteNoop);
    return true;
}

static IOFireWireLibIsochChannelRef makeChannel(IOFireWireLibDeviceRef device,
                                                 UInt32 payloadBytes) {
    return (*device)->CreateIsochChannel(
        device, true, payloadBytes, kFWSpeed400MBit,
        CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
}

static uint32_t makeConnectedIpcr(uint32_t old, UInt32 channel) {
    uint32_t v = old & ~(kPcrBroadcast | kPcrP2PMask | kPcrChannelMask);
    v |= (1u << 24);
    v |= (channel & 0x3f) << 16;
    return v;
}

static uint32_t makeConnectedOpcr(uint32_t old, UInt32 channel, IOFWSpeed speed) {
    uint32_t v = old & ~(kPcrBroadcast | kPcrP2PMask | kPcrChannelMask |
                         kOpcrXSpeedMask | kOpcrSpeedMask | kOpcrOverheadMask);
    v |= (1u << 24);
    v |= (channel & 0x3f) << 16;
    const UInt32 speedCode = static_cast<UInt32>(speed) & 0x3;
    v |= speedCode << 14;
    return v;
}

static bool restorePcr(IOFireWireLibDeviceRef device, UInt32 generation,
                       UInt16 nodeID, UInt32 lo, uint32_t original) {
    uint32_t current = 0;
    if (!readReg(device, generation, nodeID, lo, current)) return false;
    if (current == original) return true;
    return compareSwapReg(device, generation, nodeID, lo, current, original);
}

static bool run(IOFireWireLibDeviceRef device, UInt32 generation,
                UInt16 nodeID, bool execute) {
    const IOReturn openResult = (*device)->Open(device);
    if (openResult != kIOReturnSuccess) {
        std::cout << "open failed: 0x" << std::hex << openResult << std::dec << '\n';
        return false;
    }

    uint32_t opcr0 = 0, ipcr0 = 0;
    if (!readReg(device, generation, nodeID, kOpcr0Lo, opcr0) ||
        !readReg(device, generation, nodeID, kIpcr0Lo, ipcr0)) {
        (*device)->Close(device);
        return false;
    }

    std::cout << "preflight (48 kHz formation only):\n";
    printPcr("oPCR[0] device OUTPUT / host capture", opcr0);
    printPcr("iPCR[0] device INPUT / host playback", ipcr0);
    std::cout << "    capture max payload:  " << kCapturePayload48k << " bytes\n";
    std::cout << "    playback max payload: " << kPlaybackPayload48k << " bytes\n";

    if (!pcrReady(opcr0) || !pcrReady(ipcr0)) {
        std::cout << "status: REFUSED - one or both PCR0 plugs are offline or already in use\n";
        (*device)->Close(device);
        return false;
    }

    if (!execute) {
        std::cout << "status: PASS - no resources allocated and no PCR writes performed\n";
        std::cout << "to execute: ./cmpconnect --execute\n";
        (*device)->Close(device);
        return true;
    }

    PortContext captureCtx{"capture"};
    PortContext playbackCtx{"playback"};

    IOFireWireLibIsochChannelRef captureChannel = makeChannel(device, kCapturePayload48k);
    IOFireWireLibIsochChannelRef playbackChannel = makeChannel(device, kPlaybackPayload48k);
    IOFireWireLibRemoteIsochPortRef capturePort =
        (*device)->CreateRemoteIsochPort(device, true,
            CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    IOFireWireLibRemoteIsochPortRef playbackPort =
        (*device)->CreateRemoteIsochPort(device, false,
            CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));

    bool captureAllocated = false;
    bool playbackAllocated = false;
    bool opcrConnected = false;
    bool ipcrConnected = false;
    bool ok = false;

    if (!captureChannel || !playbackChannel || !capturePort || !playbackPort ||
        !configureRemotePort(capturePort, &captureCtx) ||
        !configureRemotePort(playbackPort, &playbackCtx)) {
        std::cout << "resource objects: failed to create/configure\n";
        goto cleanup;
    }

    (*captureChannel)->SetTalker(captureChannel,
        reinterpret_cast<IOFireWireLibIsochPortRef>(capturePort));
    (*playbackChannel)->AddListener(playbackChannel,
        reinterpret_cast<IOFireWireLibIsochPortRef>(playbackPort));

    {
        const IOReturn kr = (*captureChannel)->AllocateChannel(captureChannel);
        if (kr != kIOReturnSuccess || !captureCtx.allocated) {
            std::cout << "capture IRM allocation: failed (0x" << std::hex << kr << std::dec << ")\n";
            goto cleanup;
        }
        captureAllocated = true;
        std::cout << "capture IRM allocation: success, channel=" << captureCtx.channel
                  << ", speed=" << static_cast<unsigned>(captureCtx.speed) << '\n';
    }

    {
        const IOReturn kr = (*playbackChannel)->AllocateChannel(playbackChannel);
        if (kr != kIOReturnSuccess || !playbackCtx.allocated) {
            std::cout << "playback IRM allocation: failed (0x" << std::hex << kr << std::dec << ")\n";
            goto cleanup;
        }
        playbackAllocated = true;
        std::cout << "playback IRM allocation: success, channel=" << playbackCtx.channel
                  << ", speed=" << static_cast<unsigned>(playbackCtx.speed) << '\n';
    }

    {
        const uint32_t newOpcr = makeConnectedOpcr(opcr0, captureCtx.channel, captureCtx.speed);
        if (!compareSwapReg(device, generation, nodeID, kOpcr0Lo, opcr0, newOpcr))
            goto cleanup;
        opcrConnected = true;
    }

    {
        const uint32_t newIpcr = makeConnectedIpcr(ipcr0, playbackCtx.channel);
        if (!compareSwapReg(device, generation, nodeID, kIpcr0Lo, ipcr0, newIpcr))
            goto cleanup;
        ipcrConnected = true;
    }

    std::cout << "CMP establish: both PCR0 connections set\n";
    {
        uint32_t op=0, ip=0;
        if (readReg(device, generation, nodeID, kOpcr0Lo, op) &&
            readReg(device, generation, nodeID, kIpcr0Lo, ip)) {
            printPcr("oPCR[0] connected", op);
            printPcr("iPCR[0] connected", ip);
        }
    }

    std::cout << "AMDTP start: NO\n";
    usleep(500000);
    ok = true;

cleanup:
    if (ipcrConnected) {
        std::cout << "restore iPCR[0]: "
                  << (restorePcr(device, generation, nodeID, kIpcr0Lo, ipcr0) ? "success" : "failed")
                  << '\n';
    }
    if (opcrConnected) {
        std::cout << "restore oPCR[0]: "
                  << (restorePcr(device, generation, nodeID, kOpcr0Lo, opcr0) ? "success" : "failed")
                  << '\n';
    }

    if (playbackAllocated && playbackChannel) {
        const IOReturn kr = (*playbackChannel)->ReleaseChannel(playbackChannel);
        std::cout << "release playback IRM: 0x" << std::hex << kr << std::dec << '\n';
    }
    if (captureAllocated && captureChannel) {
        const IOReturn kr = (*captureChannel)->ReleaseChannel(captureChannel);
        std::cout << "release capture IRM:  0x" << std::hex << kr << std::dec << '\n';
    }

    if (playbackPort) (*playbackPort)->Release(playbackPort);
    if (capturePort) (*capturePort)->Release(capturePort);
    if (playbackChannel) (*playbackChannel)->Release(playbackChannel);
    if (captureChannel) (*captureChannel)->Release(captureChannel);

    {
        uint32_t op=0, ip=0;
        if (readReg(device, generation, nodeID, kOpcr0Lo, op) &&
            readReg(device, generation, nodeID, kIpcr0Lo, ip)) {
            std::cout << "post-test PCR state:\n";
            printPcr("oPCR[0]", op);
            printPcr("iPCR[0]", ip);
            std::cout << "    exact restore: "
                      << ((op == opcr0 && ip == ipcr0) ? "PASS" : "FAIL") << '\n';
        }
    }

    (*device)->Close(device);
    return ok;
}

} // namespace

int main(int argc, char **argv) {
    const bool execute = (argc == 2 && std::string(argv[1]) == "--execute");
    if (argc > 2 || (argc == 2 && !execute)) {
        std::cerr << "usage: " << argv[0] << " [--execute]\n";
        return 64;
    }

    std::cout << "macfw cmpconnect — guarded dual CMP connection test\n\n";

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS)
        return 1;

    bool found = false;
    bool success = false;
    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        if (!isOperationalFw410(service)) {
            IOObjectRelease(service);
            continue;
        }
        found = true;

        IOCFPlugInInterface **plugin = nullptr;
        SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(service, kIOFireWireLibTypeID,
                                              kIOCFPlugInInterfaceID,
                                              &plugin, &score) != KERN_SUCCESS || !plugin) {
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
                std::cout << "FW410 operational unit:\n";
                std::cout << "    generation: " << generation << '\n';
                std::cout << "    remote node: 0x" << std::hex << nodeID << std::dec << '\n';
                success = run(device, generation, nodeID, execute);
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
    return success ? 0 : 1;
}