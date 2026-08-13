#include "macfw/cmp.h"
#include "macfw/firewire_device.h"

#include <IOKit/firewire/IOFireWireLibIsoch.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

constexpr UInt32 kCapturePayload48k = 128;
constexpr UInt32 kPlaybackPayload48k = 272;

struct PortContext {
    const char *name = nullptr;
    bool allocated = false;
    IOFWSpeed speed = kFWSpeed100MBit;
    UInt32 channel = 0;
};

static void printPcr(const char *name, std::uint32_t value) {
    const auto state = macfw::cmp::decodePcr(value);
    std::cout << "    " << name << ": 0x" << std::hex << std::setw(8)
              << std::setfill('0') << value << std::dec << std::setfill(' ') << '\n';
    std::cout << "        online: " << (state.online ? "yes" : "no")
              << ", p2p=" << static_cast<unsigned>(state.p2pConnections)
              << ", channel=" << static_cast<unsigned>(state.channel) << '\n';
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

static bool run(macfw::FireWireDevice& fw, bool execute) {
    const IOReturn openResult = fw.open();
    if (openResult != kIOReturnSuccess) {
        std::cout << "open failed: 0x" << std::hex << openResult << std::dec << '\n';
        return false;
    }

    std::uint32_t opcr0 = 0, ipcr0 = 0;
    if (macfw::cmp::readOpcr0(fw, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(fw, ipcr0) != kIOReturnSuccess) {
        fw.close();
        return false;
    }

    std::cout << "preflight (48 kHz formation only):\n";
    printPcr("oPCR[0] device OUTPUT / host capture", opcr0);
    printPcr("iPCR[0] device INPUT / host playback", ipcr0);
    std::cout << "    capture max payload:  " << kCapturePayload48k << " bytes\n";
    std::cout << "    playback max payload: " << kPlaybackPayload48k << " bytes\n";

    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cout << "status: REFUSED - one or both PCR0 plugs are offline or already in use\n";
        fw.close();
        return false;
    }

    if (!execute) {
        std::cout << "status: PASS - no resources allocated and no PCR writes performed\n";
        std::cout << "to execute: ./cmpconnect --execute\n";
        fw.close();
        return true;
    }

    IOFireWireLibDeviceRef device = fw.nativeHandle();
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

    if (macfw::cmp::connectOpcr0(
            fw, opcr0, captureCtx.channel, captureCtx.speed) != kIOReturnSuccess)
        goto cleanup;
    opcrConnected = true;

    if (macfw::cmp::connectIpcr0(
            fw, ipcr0, playbackCtx.channel) != kIOReturnSuccess)
        goto cleanup;
    ipcrConnected = true;

    std::cout << "CMP establish: both PCR0 connections set\n";
    {
        std::uint32_t op = 0, ip = 0;
        if (macfw::cmp::readOpcr0(fw, op) == kIOReturnSuccess &&
            macfw::cmp::readIpcr0(fw, ip) == kIOReturnSuccess) {
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
                  << (macfw::cmp::restore(
                          fw, macfw::cmp::kIpcr0AddressLo, ipcr0) == kIOReturnSuccess
                          ? "success" : "failed")
                  << '\n';
    }
    if (opcrConnected) {
        std::cout << "restore oPCR[0]: "
                  << (macfw::cmp::restore(
                          fw, macfw::cmp::kOpcr0AddressLo, opcr0) == kIOReturnSuccess
                          ? "success" : "failed")
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
        std::uint32_t op = 0, ip = 0;
        if (macfw::cmp::readOpcr0(fw, op) == kIOReturnSuccess &&
            macfw::cmp::readIpcr0(fw, ip) == kIOReturnSuccess) {
            std::cout << "post-test PCR state:\n";
            printPcr("oPCR[0]", op);
            printPcr("iPCR[0]", ip);
            std::cout << "    exact restore: "
                      << ((op == opcr0 && ip == ipcr0) ? "PASS" : "FAIL") << '\n';
        }
    }

    fw.close();
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

    auto fw = macfw::FireWireDevice::findByProductName("FW 410");
    if (!fw) {
        std::cout << "No operational FW 410 unit found.\n";
        return 2;
    }

    std::cout << "FW410 operational unit:\n";
    std::cout << "    generation: " << fw.generation() << '\n';
    std::cout << "    remote node: 0x" << std::hex << fw.nodeID() << std::dec << '\n';

    return run(fw, execute) ? 0 : 1;
}
