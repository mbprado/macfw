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

#include "../common/am824.h"

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

// 48 kHz blocking-mode maximum packet sizes.
// A data-bearing packet carries 8 events.
constexpr UInt32 kCaptureMaxPacket48k =
    8 + 8 * 5 * 4;       // CIP + (8 events * (4 PCM + 1 MIDI position))
constexpr UInt32 kPlaybackMaxPacket48k =
    8 + 8 * 11 * 4;      // CIP + (8 events * (10 PCM + 1 MIDI position))

constexpr size_t kCaptureSlots = 256;
constexpr size_t kCaptureBufferBytes = kCaptureMaxPacket48k;
constexpr size_t kPlaybackSlots = 64;
constexpr size_t kPlaybackNoDataBytes = 8;

struct CaptureSlot {
    UInt32 isoHeader;
    UInt32 status;
    UInt32 timestamp;
    UInt8 payload[kCaptureBufferBytes];
};

struct PlaybackSlot {
    UInt32 status;
    UInt32 timestamp;
    UInt8 payload[kPlaybackNoDataBytes];
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
    const bool ok =
        CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringCompare(static_cast<CFStringRef>(value),
                        CFSTR("FW 410"), 0) == kCFCompareEqualTo;
    CFRelease(value);
    return ok;
}

static uint32_t be32(const UInt8 *p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

static void putBe32(UInt8 *p, uint32_t v) {
    p[0] = static_cast<UInt8>((v >> 24) & 0xff);
    p[1] = static_cast<UInt8>((v >> 16) & 0xff);
    p[2] = static_cast<UInt8>((v >> 8) & 0xff);
    p[3] = static_cast<UInt8>(v & 0xff);
}

static bool readReg(IOFireWireLibDeviceRef device, UInt32 generation,
                    UInt16 nodeID, UInt32 lo, uint32_t& value) {
    FWAddress address{};
    address.nodeID = nodeID;
    address.addressHi = kAddressHi;
    address.addressLo = lo;

    std::array<UInt8, 4> bytes{};
    UInt32 size = static_cast<UInt32>(bytes.size());
    const IOReturn kr = (*device)->Read(
        device, 0, &address, bytes.data(), &size, true, generation);
    if (kr != kIOReturnSuccess || size != 4)
        return false;

    value = be32(bytes.data());
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

    return (*device)->CompareSwap(
        device, 0, &address, expectedBus, newBus,
        true, generation) == kIOReturnSuccess;
}

static bool restoreReg(IOFireWireLibDeviceRef device, UInt32 generation,
                       UInt16 nodeID, UInt32 lo, uint32_t original) {
    uint32_t current = 0;
    if (!readReg(device, generation, nodeID, lo, current))
        return false;
    if (current == original)
        return true;
    return compareSwapReg(
        device, generation, nodeID, lo, current, original);
}

static bool pcrReady(uint32_t value) {
    return (value & kPcrOnline) != 0 &&
           (value & (kPcrBroadcast | kPcrP2PMask)) == 0;
}

static uint32_t makeConnectedIpcr(uint32_t old, UInt32 channel) {
    uint32_t v =
        old & ~(kPcrBroadcast | kPcrP2PMask | kPcrChannelMask);
    v |= (1u << 24);
    v |= (channel & 0x3f) << 16;
    return v;
}

static uint32_t makeConnectedOpcr(
    uint32_t old, UInt32 channel, IOFWSpeed speed) {
    uint32_t v =
        old & ~(kPcrBroadcast | kPcrP2PMask | kPcrChannelMask |
                kOpcrXSpeedMask | kOpcrSpeedMask | kOpcrOverheadMask);
    v |= (1u << 24);
    v |= (channel & 0x3f) << 16;
    v |= (static_cast<UInt32>(speed) & 0x3) << 14;
    return v;
}

static IOReturn remoteGetSupported(
    IOFireWireLibIsochPortRef,
    IOFWSpeed *outMaxSpeed,
    UInt64 *outChannels) {
    if (outMaxSpeed) *outMaxSpeed = kFWSpeed400MBit;
    if (outChannels) *outChannels = ~static_cast<UInt64>(0);
    return kIOReturnSuccess;
}

static IOReturn remoteAllocate(
    IOFireWireLibIsochPortRef interface,
    IOFWSpeed speed,
    UInt32 channel) {
    auto *ctx =
        static_cast<PortContext *>((*interface)->GetRefCon(interface));
    if (ctx) {
        ctx->allocated = true;
        ctx->speed = speed;
        ctx->channel = channel;
    }
    return kIOReturnSuccess;
}

static IOReturn remoteRelease(IOFireWireLibIsochPortRef interface) {
    auto *ctx =
        static_cast<PortContext *>((*interface)->GetRefCon(interface));
    if (ctx)
        ctx->allocated = false;
    return kIOReturnSuccess;
}

static IOReturn remoteNoop(IOFireWireLibIsochPortRef) {
    return kIOReturnSuccess;
}

static bool configureRemotePort(
    IOFireWireLibRemoteIsochPortRef port,
    PortContext *ctx) {
    if (!port)
        return false;

    auto base =
        reinterpret_cast<IOFireWireLibIsochPortRef>(port);
    (*base)->SetRefCon(base, ctx);
    (*port)->SetGetSupportedHandler(port, remoteGetSupported);
    (*port)->SetAllocatePortHandler(port, remoteAllocate);
    (*port)->SetReleasePortHandler(port, remoteRelease);
    (*port)->SetStartHandler(port, remoteNoop);
    (*port)->SetStopHandler(port, remoteNoop);
    return true;
}

static bool captureTouched(const CaptureSlot& p) {
    return p.isoHeader != 0 ||
           p.status != 0 ||
           p.timestamp != 0;
}

static bool dataBearing(const CaptureSlot& p) {
    const unsigned len = p.isoHeader >> 16;
    return len > 8 && len <= kCaptureBufferBytes;
}

static void dumpCapture(
    const CaptureSlot *slots, bool raw) {
    size_t touched = 0;
    size_t data = 0;
    macfw::am824::CaptureStats pcmStats;

    for (size_t i = 0; i < kCaptureSlots; ++i) {
        if (captureTouched(slots[i]))
            ++touched;
        if (dataBearing(slots[i])) {
            ++data;
            const unsigned len = slots[i].isoHeader >> 16;
            macfw::am824::accumulateCapture48k(
                slots[i].payload,
                len,
                pcmStats);
        }
    }

    std::cout << "capture summary:\n";
    std::cout << "    touched slots:      "
              << touched << " / " << kCaptureSlots << '\n';
    std::cout << "    data-bearing slots: "
              << data << '\n';

    size_t shown = 0;
    for (size_t i = 0;
         i < kCaptureSlots && shown < 16; ++i) {
        if (!captureTouched(slots[i]))
            continue;

        const unsigned len = slots[i].isoHeader >> 16;
        const unsigned tag =
            (slots[i].isoHeader >> 14) & 0x3;
        const unsigned sy = slots[i].isoHeader & 0xf;

        std::cout << "    packet " << i
                  << ": len=" << len
                  << " tag=" << tag
                  << " sy=" << sy;

        if (len >= 8 && len <= kCaptureBufferBytes) {
            const uint32_t q0 = be32(slots[i].payload);
            const uint32_t q1 = be32(slots[i].payload + 4);
            std::cout
                << " CIP{dbs=" << ((q0 >> 16) & 0xff)
                << " dbc=" << (q0 & 0xff)
                << " fmt=0x" << std::hex
                << ((q1 >> 24) & 0x3f)
                << " fdf=0x" << ((q1 >> 16) & 0xff)
                << " syt=0x" << (q1 & 0xffff)
                << std::dec << "}";
        }
        std::cout << '\n';

        if (raw &&
            len > 0 &&
            len <= kCaptureBufferBytes) {
            const unsigned n = len < 48 ? len : 48;
            std::cout << "        raw:";
            for (unsigned j = 0; j < n; ++j) {
                std::cout
                    << ' ' << std::hex
                    << std::setw(2)
                    << std::setfill('0')
                    << static_cast<unsigned>(
                           slots[i].payload[j]);
            }
            std::cout
                << std::dec << std::setfill(' ') << '\n';
        }

        ++shown;
    }

    if (data > 0) {
        std::cout
            << "result: PASS - FW410 transitioned to "
               "sample-bearing capture packets\n";
    } else if (touched > 0) {
        std::cout
            << "result: PARTIAL - duplex ISO traffic active, "
               "capture still NODATA\n";
    } else {
        std::cout
            << "result: FAIL - no capture ISO packets observed\n";
    }

    std::cout << '\n';
    macfw::am824::printCaptureStats(pcmStats, std::cout);
}

static void makePlaybackNoData(PlaybackSlot& slot) {
    std::memset(&slot, 0, sizeof(slot));

    // AM824, 48-kHz FDF, no sample payload, no SYT information.
    // DBS represents the configured 11-position playback formation.
    putBe32(slot.payload + 0, 0x000b0000u);
    putBe32(slot.payload + 4, 0x9002ffffu);
}

static bool run(
    IOFireWireLibDeviceRef device,
    UInt32 generation,
    UInt16 nodeID,
    bool execute,
    bool raw) {
    if ((*device)->Open(device) != kIOReturnSuccess) {
        std::cout << "open failed\n";
        return false;
    }

    uint32_t opcr0 = 0;
    uint32_t ipcr0 = 0;
    if (!readReg(
            device, generation, nodeID, kOpcr0Lo, opcr0) ||
        !readReg(
            device, generation, nodeID, kIpcr0Lo, ipcr0)) {
        (*device)->Close(device);
        return false;
    }

    std::cout << "preflight (48 kHz NODATA duplex):\n";
    std::cout << "    oPCR[0]: 0x"
              << std::hex << opcr0 << std::dec << '\n';
    std::cout << "    iPCR[0]: 0x"
              << std::hex << ipcr0 << std::dec << '\n';
    std::cout << "    capture max packet:  "
              << kCaptureMaxPacket48k << " bytes\n";
    std::cout << "    playback max packet: "
              << kPlaybackMaxPacket48k << " bytes\n";
    std::cout
        << "    playback stream: 8-byte AM824 NODATA packets\n";

    if (!pcrReady(opcr0) || !pcrReady(ipcr0)) {
        std::cout
            << "status: REFUSED - PCR0 offline or connected\n";
        (*device)->Close(device);
        return false;
    }

    if (!execute) {
        std::cout
            << "status: PASS - no resources or packets used\n";
        std::cout
            << "to execute: ./isoduplex --execute [--raw]\n";
        (*device)->Close(device);
        return true;
    }

    const size_t captureBytes =
        sizeof(CaptureSlot) * kCaptureSlots;
    const size_t playbackBytes =
        sizeof(PlaybackSlot) * kPlaybackSlots;

    auto *capture = static_cast<CaptureSlot *>(
        mmap(nullptr, captureBytes,
             PROT_READ | PROT_WRITE,
             MAP_ANON | MAP_SHARED, -1, 0));
    auto *playback = static_cast<PlaybackSlot *>(
        mmap(nullptr, playbackBytes,
             PROT_READ | PROT_WRITE,
             MAP_ANON | MAP_SHARED, -1, 0));

    if (capture == MAP_FAILED ||
        playback == MAP_FAILED) {
        if (capture != MAP_FAILED)
            munmap(capture, captureBytes);
        if (playback != MAP_FAILED)
            munmap(playback, playbackBytes);
        (*device)->Close(device);
        return false;
    }

    std::memset(capture, 0, captureBytes);
    for (size_t i = 0; i < kPlaybackSlots; ++i)
        makePlaybackNoData(playback[i]);

    PortContext captureRemoteCtx{};
    PortContext playbackRemoteCtx{};

    IOFireWireLibNuDCLPoolRef rxPool = nullptr;
    IOFireWireLibNuDCLPoolRef txPool = nullptr;
    IOFireWireLibLocalIsochPortRef localCapture = nullptr;
    IOFireWireLibLocalIsochPortRef localPlayback = nullptr;
    IOFireWireLibRemoteIsochPortRef captureRemote = nullptr;
    IOFireWireLibRemoteIsochPortRef playbackRemote = nullptr;
    IOFireWireLibIsochChannelRef captureChannel = nullptr;
    IOFireWireLibIsochChannelRef playbackChannel = nullptr;
    NuDCLRef rxFirst = nullptr;
    NuDCLRef rxLast = nullptr;
    NuDCLRef txFirst = nullptr;
    NuDCLRef txLast = nullptr;

    bool captureAllocated = false;
    bool playbackAllocated = false;
    bool opcrConnected = false;
    bool ipcrConnected = false;
    bool captureStarted = false;
    bool playbackStarted = false;
    bool callbackDispatcher = false;
    bool isochDispatcher = false;
    bool notifications = false;
    bool ok = false;

    rxPool = (*device)->CreateNuDCLPool(
        device,
        static_cast<UInt32>(kCaptureSlots),
        CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    txPool = (*device)->CreateNuDCLPool(
        device,
        static_cast<UInt32>(kPlaybackSlots),
        CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));

    if (!rxPool || !txPool)
        goto cleanup;

    for (size_t i = 0; i < kCaptureSlots; ++i) {
        IOVirtualRange ranges[2] = {
            {
                reinterpret_cast<IOVirtualAddress>(
                    &capture[i].isoHeader),
                4
            },
            {
                reinterpret_cast<IOVirtualAddress>(
                    capture[i].payload),
                sizeof(capture[i].payload)
            }
        };

        auto dcl = (*rxPool)->AllocateReceivePacket(
            rxPool, nullptr, 4, 2, ranges);
        if (!dcl)
            goto cleanup;

        NuDCLRef ref =
            reinterpret_cast<NuDCLRef>(dcl);
        (*rxPool)->SetDCLStatusPtr(
            ref, &capture[i].status);
        (*rxPool)->SetDCLTimeStampPtr(
            ref, &capture[i].timestamp);

        if (!rxFirst)
            rxFirst = ref;
        rxLast = ref;
    }

    if ((*rxPool)->SetDCLBranch(
            rxLast, rxFirst) != kIOReturnSuccess)
        goto cleanup;

    (*txPool)->SetCurrentTagAndSync(txPool, 1, 0);

    for (size_t i = 0; i < kPlaybackSlots; ++i) {
        IOVirtualRange range = {
            reinterpret_cast<IOVirtualAddress>(
                playback[i].payload),
            sizeof(playback[i].payload)
        };

        auto dcl = (*txPool)->AllocateSendPacket(
            txPool, nullptr, 1, &range);
        if (!dcl)
            goto cleanup;

        NuDCLRef ref =
            reinterpret_cast<NuDCLRef>(dcl);
        (*txPool)->SetDCLStatusPtr(
            ref, &playback[i].status);
        (*txPool)->SetDCLTimeStampPtr(
            ref, &playback[i].timestamp);

        if (!txFirst)
            txFirst = ref;
        txLast = ref;
    }

    if ((*txPool)->SetDCLBranch(
            txLast, txFirst) != kIOReturnSuccess)
        goto cleanup;

    {
        DCLCommand *program =
            (*rxPool)->GetProgram(rxPool);
        IOVirtualRange mapped = {
            reinterpret_cast<IOVirtualAddress>(capture),
            captureBytes
        };

        localCapture =
            (*device)->CreateLocalIsochPort(
                device, false, program,
                0, 0, 0,
                nullptr, 0,
                &mapped, 1,
                CFUUIDGetUUIDBytes(
                    kIOFireWireLocalIsochPortInterfaceID));

        if (!localCapture)
            goto cleanup;
    }

    {
        DCLCommand *program =
            (*txPool)->GetProgram(txPool);
        IOVirtualRange mapped = {
            reinterpret_cast<IOVirtualAddress>(playback),
            playbackBytes
        };

        localPlayback =
            (*device)->CreateLocalIsochPort(
                device, true, program,
                0, 0, 0,
                nullptr, 0,
                &mapped, 1,
                CFUUIDGetUUIDBytes(
                    kIOFireWireLocalIsochPortInterfaceID));

        if (!localPlayback)
            goto cleanup;
    }

    captureRemote =
        (*device)->CreateRemoteIsochPort(
            device, true,
            CFUUIDGetUUIDBytes(
                kIOFireWireRemoteIsochPortInterfaceID));
    playbackRemote =
        (*device)->CreateRemoteIsochPort(
            device, false,
            CFUUIDGetUUIDBytes(
                kIOFireWireRemoteIsochPortInterfaceID));

    if (!captureRemote ||
        !playbackRemote ||
        !configureRemotePort(
            captureRemote, &captureRemoteCtx) ||
        !configureRemotePort(
            playbackRemote, &playbackRemoteCtx))
        goto cleanup;

    captureChannel =
        (*device)->CreateIsochChannel(
            device, true,
            kCaptureMaxPacket48k,
            kFWSpeed400MBit,
            CFUUIDGetUUIDBytes(
                kIOFireWireIsochChannelInterfaceID));
    playbackChannel =
        (*device)->CreateIsochChannel(
            device, true,
            kPlaybackMaxPacket48k,
            kFWSpeed400MBit,
            CFUUIDGetUUIDBytes(
                kIOFireWireIsochChannelInterfaceID));

    if (!captureChannel || !playbackChannel)
        goto cleanup;

    (*captureChannel)->SetTalker(
        captureChannel,
        reinterpret_cast<IOFireWireLibIsochPortRef>(
            captureRemote));
    (*captureChannel)->AddListener(
        captureChannel,
        reinterpret_cast<IOFireWireLibIsochPortRef>(
            localCapture));

    (*playbackChannel)->SetTalker(
        playbackChannel,
        reinterpret_cast<IOFireWireLibIsochPortRef>(
            localPlayback));
    (*playbackChannel)->AddListener(
        playbackChannel,
        reinterpret_cast<IOFireWireLibIsochPortRef>(
            playbackRemote));

    if ((*device)->AddCallbackDispatcherToRunLoop(
            device, CFRunLoopGetCurrent()) ==
        kIOReturnSuccess)
        callbackDispatcher = true;

    if ((*device)->AddIsochCallbackDispatcherToRunLoop(
            device, CFRunLoopGetCurrent()) ==
        kIOReturnSuccess)
        isochDispatcher = true;

    if ((*device)->TurnOnNotification(device))
        notifications = true;

    if ((*captureChannel)->AllocateChannel(
            captureChannel) != kIOReturnSuccess ||
        !captureRemoteCtx.allocated)
        goto cleanup;
    captureAllocated = true;

    if ((*playbackChannel)->AllocateChannel(
            playbackChannel) != kIOReturnSuccess ||
        !playbackRemoteCtx.allocated)
        goto cleanup;
    playbackAllocated = true;

    std::cout << "ISO resources:\n";
    std::cout << "    capture channel:  "
              << captureRemoteCtx.channel << '\n';
    std::cout << "    playback channel: "
              << playbackRemoteCtx.channel << '\n';

    {
        const uint32_t connected =
            makeConnectedOpcr(
                opcr0,
                captureRemoteCtx.channel,
                captureRemoteCtx.speed);
        if (!compareSwapReg(
                device, generation, nodeID,
                kOpcr0Lo, opcr0, connected))
            goto cleanup;
        opcrConnected = true;
    }

    {
        const uint32_t connected =
            makeConnectedIpcr(
                ipcr0, playbackRemoteCtx.channel);
        if (!compareSwapReg(
                device, generation, nodeID,
                kIpcr0Lo, ipcr0, connected))
            goto cleanup;
        ipcrConnected = true;
    }

    // Linux starts host playback (AMDTP OUT) before capture.
    if ((*playbackChannel)->Start(
            playbackChannel) != kIOReturnSuccess)
        goto cleanup;
    playbackStarted = true;

    if ((*captureChannel)->Start(
            captureChannel) != kIOReturnSuccess)
        goto cleanup;
    captureStarted = true;

    std::cout
        << "duplex ISO: started "
           "(playback NODATA + capture receive)\n";
    std::cout
        << "observation window: 2.0 s\n";

    {
        const CFTimeInterval end =
            CFAbsoluteTimeGetCurrent() + 2.0;
        while (CFAbsoluteTimeGetCurrent() < end) {
            CFRunLoopRunInMode(
                kCFRunLoopDefaultMode,
                0.05, true);
        }
    }

    dumpCapture(capture, raw);
    ok = true;

cleanup:
    if (captureStarted && captureChannel)
        (*captureChannel)->Stop(captureChannel);
    if (playbackStarted && playbackChannel)
        (*playbackChannel)->Stop(playbackChannel);

    if (ipcrConnected) {
        std::cout
            << "restore iPCR[0]: "
            << (restoreReg(
                    device, generation, nodeID,
                    kIpcr0Lo, ipcr0)
                    ? "success" : "failed")
            << '\n';
    }

    if (opcrConnected) {
        std::cout
            << "restore oPCR[0]: "
            << (restoreReg(
                    device, generation, nodeID,
                    kOpcr0Lo, opcr0)
                    ? "success" : "failed")
            << '\n';
    }

    if (playbackAllocated && playbackChannel)
        (*playbackChannel)->ReleaseChannel(
            playbackChannel);
    if (captureAllocated && captureChannel)
        (*captureChannel)->ReleaseChannel(
            captureChannel);

    if (notifications)
        (*device)->TurnOffNotification(device);
    if (isochDispatcher)
        (*device)->RemoveIsochCallbackDispatcherFromRunLoop(
            device);
    if (callbackDispatcher)
        (*device)->RemoveCallbackDispatcherFromRunLoop(
            device);

    if (playbackRemote)
        (*playbackRemote)->Release(playbackRemote);
    if (captureRemote)
        (*captureRemote)->Release(captureRemote);
    if (localPlayback)
        (*localPlayback)->Release(localPlayback);
    if (localCapture)
        (*localCapture)->Release(localCapture);
    if (playbackChannel)
        (*playbackChannel)->Release(playbackChannel);
    if (captureChannel)
        (*captureChannel)->Release(captureChannel);
    if (txPool)
        (*txPool)->Release(txPool);
    if (rxPool)
        (*rxPool)->Release(rxPool);

    uint32_t opAfter = 0;
    uint32_t ipAfter = 0;
    if (readReg(
            device, generation, nodeID,
            kOpcr0Lo, opAfter) &&
        readReg(
            device, generation, nodeID,
            kIpcr0Lo, ipAfter)) {
        std::cout
            << "post-test PCR restore: "
            << ((opAfter == opcr0 &&
                 ipAfter == ipcr0)
                    ? "PASS" : "MISMATCH")
            << '\n';
    }

    munmap(playback, playbackBytes);
    munmap(capture, captureBytes);
    (*device)->Close(device);
    return ok;
}

} // namespace

int main(int argc, char **argv) {
    bool execute = false;
    bool raw = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute")
            execute = true;
        else if (arg == "--raw")
            raw = true;
        else {
            std::cerr
                << "usage: " << argv[0]
                << " [--execute] [--raw]\n";
            return 64;
        }
    }

    std::cout
        << "macfw isoduplex — guarded FW410 "
           "duplex AMDTP transport test\n\n";

    CFMutableDictionaryRef matching =
        IOServiceMatching("IOFireWireUnit");
    if (!matching)
        return 1;

    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(
            kIOMainPortDefault,
            matching,
            &iterator) != KERN_SUCCESS)
        return 1;

    bool found = false;
    bool success = false;
    io_registry_entry_t service = IO_OBJECT_NULL;

    while ((service = IOIteratorNext(iterator)) !=
           IO_OBJECT_NULL) {
        if (!isOperationalFw410(service)) {
            IOObjectRelease(service);
            continue;
        }

        found = true;

        IOCFPlugInInterface **plugin = nullptr;
        SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(
                service,
                kIOFireWireLibTypeID,
                kIOCFPlugInInterfaceID,
                &plugin,
                &score) != KERN_SUCCESS ||
            !plugin) {
            IOObjectRelease(service);
            continue;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr =
            (*plugin)->QueryInterface(
                plugin,
                CFUUIDGetUUIDBytes(
                    kIOFireWireDeviceInterfaceID),
                reinterpret_cast<LPVOID *>(&device));

        if (hr == 0 && device) {
            UInt32 generation = 0;
            UInt16 nodeID = 0;

            if ((*device)->GetBusGeneration(
                    device, &generation) ==
                    kIOReturnSuccess &&
                (*device)->GetRemoteNodeID(
                    device, generation, &nodeID) ==
                    kIOReturnSuccess) {
                std::cout
                    << "FW410 operational unit:\n";
                std::cout
                    << "    generation: "
                    << generation << '\n';
                std::cout
                    << "    remote node: 0x"
                    << std::hex << nodeID
                    << std::dec << '\n';

                success =
                    run(device, generation, nodeID,
                        execute, raw);
            }

            (*device)->Release(device);
        }

        IODestroyPlugInInterface(plugin);
        IOObjectRelease(service);
    }

    IOObjectRelease(iterator);

    if (!found) {
        std::cout
            << "No operational FW 410 unit found.\n";
        return 2;
    }

    return success ? 0 : 1;
}
