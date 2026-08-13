#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <libkern/OSByteOrder.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/mman.h>

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

constexpr UInt32 kCapturePayload48k = 128;   // 5 positions * 6 frames * 4 + 8 CIP
constexpr UInt32 kPlaybackPayload48k = 272;  // 11 positions * 6 frames * 4 + 8 CIP
constexpr size_t kPacketCount = 64;
constexpr size_t kPacketBufferBytes = kCapturePayload48k;

struct PacketSlot {
    UInt32 isoHeader;
    UInt32 status;
    UInt32 timestamp;
    UInt8 payload[kPacketBufferBytes];
};

struct CaptureContext {
    bool done = false;
};

struct PortContext {
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

static uint32_t be32(const UInt8 *p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

static uint32_t be32(const std::array<UInt8,4>& b) {
    return be32(b.data());
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

static bool restoreReg(IOFireWireLibDeviceRef device, UInt32 generation,
                       UInt16 nodeID, UInt32 lo, uint32_t original) {
    uint32_t current = 0;
    if (!readReg(device, generation, nodeID, lo, current)) return false;
    if (current == original) return true;
    return compareSwapReg(device, generation, nodeID, lo, current, original);
}

static bool pcrReady(uint32_t value) {
    return (value & kPcrOnline) != 0 &&
           (value & (kPcrBroadcast | kPcrP2PMask)) == 0;
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
    v |= (static_cast<UInt32>(speed) & 0x3) << 14;
    return v;
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
    auto base = reinterpret_cast<IOFireWireLibIsochPortRef>(port);
    (*base)->SetRefCon(base, ctx);
    (*port)->SetGetSupportedHandler(port, remoteGetSupported);
    (*port)->SetAllocatePortHandler(port, remoteAllocate);
    (*port)->SetReleasePortHandler(port, remoteRelease);
    (*port)->SetStartHandler(port, remoteNoop);
    (*port)->SetStopHandler(port, remoteNoop);
    return true;
}

static void captureDone(CaptureContext *ctx, NuDCLRef) {
    if (ctx) ctx->done = true;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

static void dumpPacket(size_t index, const PacketSlot& p, bool raw) {
    const UInt32 iso = p.isoHeader;
    const unsigned packetLen = iso >> 16;
    const unsigned tag = (iso >> 14) & 0x3;
    const unsigned sy = iso & 0xf;

    std::cout << "    packet " << index
              << ": len=" << packetLen
              << " tag=" << tag
              << " sy=" << sy
              << " status=0x" << std::hex << p.status
              << " timestamp=0x" << p.timestamp << std::dec;

    if (packetLen >= 8 && packetLen <= kPacketBufferBytes) {
        const uint32_t q0 = be32(p.payload);
        const uint32_t q1 = be32(p.payload + 4);
        const unsigned sid = (q0 >> 24) & 0x3f;
        const unsigned dbs = (q0 >> 16) & 0xff;
        const unsigned dbc = q0 & 0xff;
        const unsigned fmt = (q1 >> 24) & 0x3f;
        const unsigned fdf = (q1 >> 16) & 0xff;
        const unsigned syt = q1 & 0xffff;
        std::cout << " CIP{sid=" << sid
                  << " dbs=" << dbs
                  << " dbc=" << dbc
                  << " fmt=0x" << std::hex << fmt
                  << " fdf=0x" << fdf
                  << " syt=0x" << syt << std::dec << "}";
    }
    std::cout << '\n';

    if (raw && packetLen > 0 && packetLen <= kPacketBufferBytes) {
        const unsigned shown = packetLen < 48 ? packetLen : 48;
        std::cout << "        raw:";
        for (unsigned i = 0; i < shown; ++i) {
            std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(p.payload[i]);
        }
        std::cout << std::dec << std::setfill(' ');
        if (shown < packetLen) std::cout << " ...";
        std::cout << '\n';
    }
}

static bool run(IOFireWireLibDeviceRef device, UInt32 generation,
                UInt16 nodeID, bool execute, bool raw) {
    if ((*device)->Open(device) != kIOReturnSuccess) {
        std::cout << "open failed\n";
        return false;
    }

    uint32_t opcr0 = 0, ipcr0 = 0;
    if (!readReg(device, generation, nodeID, kOpcr0Lo, opcr0) ||
        !readReg(device, generation, nodeID, kIpcr0Lo, ipcr0)) {
        (*device)->Close(device);
        return false;
    }

    std::cout << "preflight (48 kHz capture):\n";
    std::cout << "    oPCR[0]: 0x" << std::hex << opcr0 << std::dec << '\n';
    std::cout << "    iPCR[0]: 0x" << std::hex << ipcr0 << std::dec << '\n';
    std::cout << "    packet buffer: " << kPacketBufferBytes << " bytes\n";
    std::cout << "    packet slots:  " << kPacketCount << '\n';

    if (!pcrReady(opcr0) || !pcrReady(ipcr0)) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n";
        (*device)->Close(device);
        return false;
    }

    if (!execute) {
        std::cout << "status: PASS - no ISO resources allocated and no stream started\n";
        std::cout << "to execute: ./isocapture --execute [--raw]\n";
        (*device)->Close(device);
        return true;
    }

    const size_t mapBytes = sizeof(PacketSlot) * kPacketCount;
    auto *slots = static_cast<PacketSlot *>(mmap(nullptr, mapBytes,
        PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0));
    if (slots == MAP_FAILED) {
        std::cout << "mmap failed\n";
        (*device)->Close(device);
        return false;
    }
    std::memset(slots, 0, mapBytes);

    CaptureContext captureState{};
    PortContext captureRemoteCtx{};
    PortContext playbackRemoteCtx{};

    IOFireWireLibNuDCLPoolRef pool = nullptr;
    IOFireWireLibLocalIsochPortRef localCapture = nullptr;
    IOFireWireLibRemoteIsochPortRef captureRemote = nullptr;
    IOFireWireLibRemoteIsochPortRef playbackRemote = nullptr;
    IOFireWireLibIsochChannelRef captureChannel = nullptr;
    IOFireWireLibIsochChannelRef playbackChannel = nullptr;
    NuDCLRef last = nullptr;

    bool captureAllocated = false;
    bool playbackAllocated = false;
    bool opcrConnected = false;
    bool ipcrConnected = false;
    bool captureStarted = false;
    bool callbackDispatcher = false;
    bool isochDispatcher = false;
    bool notifications = false;
    bool ok = false;

    pool = (*device)->CreateNuDCLPool(device, static_cast<UInt32>(kPacketCount),
        CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    if (!pool) {
        std::cout << "CreateNuDCLPool: failed\n";
        goto cleanup;
    }

    for (size_t i = 0; i < kPacketCount; ++i) {
        IOVirtualRange ranges[2] = {
            {reinterpret_cast<IOVirtualAddress>(&slots[i].isoHeader), sizeof(slots[i].isoHeader)},
            {reinterpret_cast<IOVirtualAddress>(slots[i].payload), sizeof(slots[i].payload)}
        };
        NuDCLReceivePacketRef dcl = (*pool)->AllocateReceivePacket(
            pool, nullptr, 4, 2, ranges);
        if (!dcl) {
            std::cout << "AllocateReceivePacket failed at slot " << i << '\n';
            goto cleanup;
        }
        (*pool)->SetDCLStatusPtr(reinterpret_cast<NuDCLRef>(dcl), &slots[i].status);
        (*pool)->SetDCLTimeStampPtr(reinterpret_cast<NuDCLRef>(dcl), &slots[i].timestamp);
        last = reinterpret_cast<NuDCLRef>(dcl);
    }

    (*pool)->SetDCLRefcon(last, &captureState);
    (*pool)->SetDCLCallback(last, reinterpret_cast<NuDCLCallback>(captureDone));

    {
        DCLCommand *program = (*pool)->GetProgram(pool);
        if (!program) {
            std::cout << "NuDCL program: unavailable\n";
            goto cleanup;
        }
        IOVirtualRange mappedRange = {
            reinterpret_cast<IOVirtualAddress>(slots), mapBytes
        };
        localCapture = (*device)->CreateLocalIsochPort(
            device, false, program,
            kFWDCLSyBitsEvent, 0, 0,
            nullptr, 0,
            &mappedRange, 1,
            CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
        if (!localCapture) {
            std::cout << "CreateLocalIsochPort: failed\n";
            goto cleanup;
        }
    }

    captureRemote = (*device)->CreateRemoteIsochPort(device, true,
        CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    playbackRemote = (*device)->CreateRemoteIsochPort(device, false,
        CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    if (!captureRemote || !playbackRemote ||
        !configureRemotePort(captureRemote, &captureRemoteCtx) ||
        !configureRemotePort(playbackRemote, &playbackRemoteCtx)) {
        std::cout << "remote ISO port setup: failed\n";
        goto cleanup;
    }

    captureChannel = (*device)->CreateIsochChannel(device, true,
        kCapturePayload48k, kFWSpeed400MBit,
        CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    playbackChannel = (*device)->CreateIsochChannel(device, true,
        kPlaybackPayload48k, kFWSpeed400MBit,
        CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    if (!captureChannel || !playbackChannel) {
        std::cout << "CreateIsochChannel: failed\n";
        goto cleanup;
    }

    (*captureChannel)->SetTalker(captureChannel,
        reinterpret_cast<IOFireWireLibIsochPortRef>(captureRemote));
    (*captureChannel)->AddListener(captureChannel,
        reinterpret_cast<IOFireWireLibIsochPortRef>(localCapture));
    (*playbackChannel)->AddListener(playbackChannel,
        reinterpret_cast<IOFireWireLibIsochPortRef>(playbackRemote));

    if ((*device)->AddCallbackDispatcherToRunLoop(device, CFRunLoopGetCurrent()) == kIOReturnSuccess)
        callbackDispatcher = true;
    if ((*device)->AddIsochCallbackDispatcherToRunLoop(device, CFRunLoopGetCurrent()) == kIOReturnSuccess)
        isochDispatcher = true;
    if ((*device)->TurnOnNotification(device)) notifications = true;

    {
        const IOReturn kr = (*captureChannel)->AllocateChannel(captureChannel);
        if (kr != kIOReturnSuccess || !captureRemoteCtx.allocated) {
            std::cout << "capture IRM allocation failed: 0x" << std::hex << kr << std::dec << '\n';
            goto cleanup;
        }
        captureAllocated = true;
        std::cout << "capture ISO resource: channel=" << captureRemoteCtx.channel
                  << " speed=" << static_cast<unsigned>(captureRemoteCtx.speed) << '\n';
    }

    {
        const IOReturn kr = (*playbackChannel)->AllocateChannel(playbackChannel);
        if (kr != kIOReturnSuccess || !playbackRemoteCtx.allocated) {
            std::cout << "playback IRM allocation failed: 0x" << std::hex << kr << std::dec << '\n';
            goto cleanup;
        }
        playbackAllocated = true;
        std::cout << "playback ISO resource: channel=" << playbackRemoteCtx.channel
                  << " speed=" << static_cast<unsigned>(playbackRemoteCtx.speed) << '\n';
    }

    {
        const uint32_t connected = makeConnectedOpcr(opcr0,
            captureRemoteCtx.channel, captureRemoteCtx.speed);
        if (!compareSwapReg(device, generation, nodeID, kOpcr0Lo, opcr0, connected))
            goto cleanup;
        opcrConnected = true;
    }
    {
        const uint32_t connected = makeConnectedIpcr(ipcr0, playbackRemoteCtx.channel);
        if (!compareSwapReg(device, generation, nodeID, kIpcr0Lo, ipcr0, connected))
            goto cleanup;
        ipcrConnected = true;
    }

    std::cout << "CMP: both directions connected\n";
    {
        const IOReturn kr = (*captureChannel)->Start(captureChannel);
        if (kr != kIOReturnSuccess) {
            std::cout << "capture channel start failed: 0x" << std::hex << kr << std::dec << '\n';
            goto cleanup;
        }
        captureStarted = true;
    }

    std::cout << "capture: waiting for " << kPacketCount << " ISO packets (2 s timeout)\n";
    {
        const CFTimeInterval deadline = CFAbsoluteTimeGetCurrent() + 2.0;
        while (!captureState.done && CFAbsoluteTimeGetCurrent() < deadline) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
        }
    }

    if (!captureState.done) {
        std::cout << "capture: timeout before NuDCL burst completed\n";
        goto cleanup;
    }

    std::cout << "capture: completed\n";
    for (size_t i = 0; i < kPacketCount && i < 16; ++i)
        dumpPacket(i, slots[i], raw);
    if (kPacketCount > 16)
        std::cout << "    ... " << (kPacketCount - 16) << " additional packet slots captured\n";

    ok = true;

cleanup:
    if (captureStarted && captureChannel)
        (*captureChannel)->Stop(captureChannel);

    if (ipcrConnected) {
        std::cout << "restore iPCR[0]: "
                  << (restoreReg(device, generation, nodeID, kIpcr0Lo, ipcr0) ? "success" : "failed")
                  << '\n';
    }
    if (opcrConnected) {
        std::cout << "restore oPCR[0]: "
                  << (restoreReg(device, generation, nodeID, kOpcr0Lo, opcr0) ? "success" : "failed")
                  << '\n';
    }

    if (playbackAllocated && playbackChannel)
        (*playbackChannel)->ReleaseChannel(playbackChannel);
    if (captureAllocated && captureChannel)
        (*captureChannel)->ReleaseChannel(captureChannel);

    if (notifications) (*device)->TurnOffNotification(device);
    if (isochDispatcher)
        (*device)->RemoveIsochCallbackDispatcherFromRunLoop(device);
    if (callbackDispatcher)
        (*device)->RemoveCallbackDispatcherFromRunLoop(device);

    if (playbackRemote) (*playbackRemote)->Release(playbackRemote);
    if (captureRemote) (*captureRemote)->Release(captureRemote);
    if (localCapture) (*localCapture)->Release(localCapture);
    if (playbackChannel) (*playbackChannel)->Release(playbackChannel);
    if (captureChannel) (*captureChannel)->Release(captureChannel);
    if (pool) (*pool)->Release(pool);

    uint32_t opAfter = 0, ipAfter = 0;
    if (readReg(device, generation, nodeID, kOpcr0Lo, opAfter) &&
        readReg(device, generation, nodeID, kIpcr0Lo, ipAfter)) {
        std::cout << "post-test PCR restore: "
                  << ((opAfter == opcr0 && ipAfter == ipcr0) ? "PASS" : "MISMATCH") << '\n';
    }

    munmap(slots, mapBytes);
    (*device)->Close(device);
    return ok;
}

} // namespace

int main(int argc, char **argv) {
    bool execute = false;
    bool raw = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else if (arg == "--raw") raw = true;
        else {
            std::cerr << "usage: " << argv[0] << " [--execute] [--raw]\n";
            return 64;
        }
    }

    std::cout << "macfw isocapture — guarded FW410 AMDTP capture sniffer\n\n";

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
                success = run(device, generation, nodeID, execute, raw);
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
