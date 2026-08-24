#include "macfw/amdtp_pcm_stream.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"
#include "macfw/pcm_ring_buffer.h"
#include "macfw_hal_shm.h"
#include "../capture_shared.h"
#include "../full_duplex_shared.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace macfw::transport::duplex;
constexpr UInt32 kCycleLead = 2048;

constexpr UInt16 kFcpAddressHi = 0xffff;
constexpr UInt32 kFcpCommandLo = 0xf0000b00;
constexpr UInt32 kFcpResponseLo = 0xf0000d00;
constexpr UInt32 kFcpResponseSize = 0x200;
constexpr double kFcpTimeoutSeconds = 1.0;

volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }

class FcpRateReassertion {
public:
    ~FcpRateReassertion() { reset(); }
    bool arm(macfw::FireWireDevice& device) {
        native_ = device.nativeHandle(); generation_ = device.generation(); node_ = device.nodeID();
        response_.expectedNode = node_;
        responseSpace_ = (*native_)->CreateInitialUnitsPseudoAddressSpace(
            native_, kFcpResponseLo, kFcpResponseSize, &response_, 1024, nullptr,
            kFWAddressSpaceNoReadAccess | kFWAddressSpaceShareIfExists,
            CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
        if (!responseSpace_) return false;
        (*responseSpace_)->SetWriteHandler(responseSpace_, responseHandler);
        if (!(*responseSpace_)->TurnOnNotification(responseSpace_)) { reset(); return false; }
        notificationOn_ = true; return true;
    }
    bool reassert44100() {
        const bool out = setRate(0x18), in = setRate(0x19);
        std::cout << "post-start AV/C reassert:\n"
                  << "    OUTPUT plug 0 -> 44100: " << (out ? "accepted" : "failed") << '\n'
                  << "    INPUT plug 0  -> 44100: " << (in ? "accepted" : "failed") << '\n';
        return out && in;
    }
    void reset() {
        if (responseSpace_) {
            if (notificationOn_) (*responseSpace_)->TurnOffNotification(responseSpace_);
            (*responseSpace_)->Release(responseSpace_);
        }
        responseSpace_ = nullptr; notificationOn_ = false; native_ = nullptr;
    }
private:
    struct ResponseContext {
        UInt16 expectedNode = 0; bool received = false; UInt32 length = 0;
        std::array<UInt8, kFcpResponseSize> bytes{};
    };
    static UInt32 responseHandler(IOFireWireLibPseudoAddressSpaceRef space, FWClientCommandID commandID,
                                  UInt32 packetLen, void* packet, UInt16 srcNodeID,
                                  UInt32, UInt32, void* refCon) {
        auto* ctx = static_cast<ResponseContext*>(refCon);
        if (ctx && packet && srcNodeID == ctx->expectedNode) {
            ctx->length = std::min<UInt32>(packetLen, ctx->bytes.size());
            std::memcpy(ctx->bytes.data(), packet, ctx->length); ctx->received = true;
        }
        (*space)->ClientCommandIsComplete(space, commandID, kIOReturnSuccess);
        return kIOReturnSuccess;
    }
    bool transaction(const UInt8* cmd, UInt32 len) {
        response_.received = false; response_.length = 0; response_.bytes.fill(0);
        FWAddress a{}; a.nodeID = node_; a.addressHi = kFcpAddressHi; a.addressLo = kFcpCommandLo;
        UInt32 size = len;
        if ((*native_)->Write(native_, 0, &a, cmd, &size, true, generation_) != kIOReturnSuccess) return false;
        const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kFcpTimeoutSeconds;
        while (!response_.received && CFAbsoluteTimeGetCurrent() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        return response_.received;
    }
    bool setRate(UInt8 opcode) {
        const UInt8 cmd[8] = {0x00,0xff,opcode,0x00,0x90,0x01,0xff,0xff};
        if (!transaction(cmd,sizeof(cmd))) return false;
        const UInt8 r = response_.length ? response_.bytes[0] : 0;
        const bool accepted = r==0x09 || r==0x0c || r==0x0d || r==0x0f;
        return response_.length>=8 && accepted && response_.bytes[1]==0xff &&
               response_.bytes[2]==opcode && response_.bytes[3]==0x00 &&
               response_.bytes[4]==0x90 && (response_.bytes[5]&0x07)==0x01;
    }
    IOFireWireLibDeviceRef native_ = nullptr;
    IOFireWireLibPseudoAddressSpaceRef responseSpace_ = nullptr;
    ResponseContext response_{}; UInt32 generation_ = 0; UInt16 node_ = 0; bool notificationOn_ = false;
};

bool run() {
    SharedPlaybackReader input;
    if (!input.open()) {
        std::cerr << "shared HAL ring not available; install/restart the HAL plug-in first\n";
        return false;
    }
    if (input.ring()->sampleRate.load(std::memory_order_acquire) != 44100) {
        std::cerr << "HAL device must be set to 44100 Hz for this milestone\n";
        return false;
    }
    input.discardBacklog();

    macfw::transport::CaptureSharedWriter captureShared;
    if (!captureShared.open(44100)) {
        std::cerr << "capture shared ring setup failed\n";
        return false;
    }
    // IEC 61883-6 AM824 SFC/FDF: 0x01 = 44.1 kHz.
    macfw::transport::CaptureReceivePump capturePump(0x01, true);

    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) { std::cerr << "No operational FW 410 unit found.\n"; return false; }
    if (device.open()!=kIOReturnSuccess) return false;
    std::uint32_t opcr0=0,ipcr0=0;
    if (macfw::cmp::readOpcr0(device,opcr0)!=kIOReturnSuccess || macfw::cmp::readIpcr0(device,ipcr0)!=kIOReturnSuccess) return false;
    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) || !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cerr << "PCR0 offline or already connected\n"; return false;
    }

    auto native=device.nativeHandle(); UInt32 ct=0;
    if ((*native)->GetCycleTime(native,&ct)!=kIOReturnSuccess) return false;
    const UInt32 initialCycle=cycleCount(ct), firstCycle=(initialCycle+kCycleLead)%kCyclesPerSecond;

    macfw::PcmRingBuffer pcm(kPcmCapacityFrames,kPcmChannels);
    auto rx=macfw::AmdtpReceiveRing::create(device,kCaptureSlots,kCaptureMaxPacket);
    auto tx=macfw::AmdtpTransmitRing::createSilence44100(device,firstCycle,kPlaybackSlots);
    auto capture=macfw::IsochAllocation::create(device,macfw::IsochAllocation::Direction::DeviceToHost,kCaptureMaxPacket);
    auto playback=macfw::IsochAllocation::create(device,macfw::IsochAllocation::Direction::HostToDevice,kPlaybackMaxPacket);
    if (!pcm.valid() || !rx || !tx || !capture || !playback) return false;
    macfw::AmdtpPcmStream44100 streamer(tx,pcm,initialCycle,firstCycle,kHalfPackets);
    if (!streamer.valid() || !streamer.prime()) return false;

    (*capture.nativeChannel())->AddListener(capture.nativeChannel(),reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
    if (playback.bindHostToDeviceTalkerFirst(tx.nativeLocalPort())!=kIOReturnSuccess) return false;

    FcpRateReassertion fcp;
    bool cb=false,iso=false,notif=false,capStart=false,playStart=false,opConn=false,ipConn=false;
    bool captureReady=false,ok=false;
    if ((*native)->AddCallbackDispatcherToRunLoop(native,CFRunLoopGetCurrent())==kIOReturnSuccess) cb=true;
    if (!cb || !fcp.arm(device)) goto cleanup;
    if ((*native)->AddIsochCallbackDispatcherToRunLoop(native,CFRunLoopGetCurrent())==kIOReturnSuccess) iso=true;
    if ((*native)->TurnOnNotification(native)) notif=true;
    if (capture.allocate()!=kIOReturnSuccess || playback.allocate()!=kIOReturnSuccess) goto cleanup;
    if (macfw::cmp::connectOpcr0(device,opcr0,capture.channel(),capture.speed())!=kIOReturnSuccess) goto cleanup; opConn=true;
    if (macfw::cmp::connectIpcr0(device,ipcr0,playback.channel())!=kIOReturnSuccess) goto cleanup; ipConn=true;
    if ((*playback.nativeChannel())->Start(playback.nativeChannel())!=kIOReturnSuccess) goto cleanup; playStart=true;
    if ((*capture.nativeChannel())->Start(capture.nativeChannel())!=kIOReturnSuccess) goto cleanup; capStart=true;

    {
        UInt32 nowCt=0; if ((*native)->GetCycleTime(native,&nowCt)!=kIOReturnSuccess) goto cleanup;
        const UInt32 now=cycleCount(nowCt), forward=(firstCycle+kCyclesPerSecond-now)%kCyclesPerSecond;
        if (forward>4096u) goto cleanup;
        const double wait=static_cast<double>(forward)/kCyclesPerSecond+0.020;
        std::cout << "duplex ISO started; waiting " << std::fixed << std::setprecision(3) << wait
                  << " s before 44.1 reassert\n" << std::defaultfloat;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,wait,false);
    }
    if (!fcp.reassert44100()) goto cleanup;

    {
        std::vector<float> audio(4096*macfw::hal::kChannels,0.0f);
        std::vector<std::int32_t> mapped(4096*kPcmChannels,0);
        std::uint64_t lastWrite=input.ring()->writeFrame.load(std::memory_order_acquire);
        std::uint64_t lastCaptureFrames=0;
        CFAbsoluteTime lastStatus=CFAbsoluteTimeGetCurrent();
        std::cout << "CoreAudio outputs: Analog 1-8, S/PDIF L/R (10 channels)\n"
                  << "CoreAudio inputs: Analog In 1-2, S/PDIF In L/R (4 channels)\n"
                  << "capture prefill: " << kCapturePrefillFrames << " frames (~93 ms)\n"
                  << "capture receive: terminal-slot completed 32-cycle chunks\n"
                  << "HAL bridge active: full-duplex 44.1 kHz playback + capture; Ctrl-C to stop\n";
        while (!gStopRequested) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.001,false);

            // RX first: copy completed receive groups before doing playback
            // conversion/refill work, matching the proven 48 kHz full-duplex
            // servicing order.
            capturePump.service(rx,*captureShared.ring());

            // Preserve the known-good 44.1 playback path and scheduler.
            pumpPlayback(*input.ring(),pcm,audio,mapped);
            UInt32 nowCt=0;
            if ((*native)->GetCycleTime(native,&nowCt)==kIOReturnSuccess)
                streamer.service(cycleCount(nowCt));

            // A second idempotent RX pass minimizes the DMA reuse window.
            capturePump.service(rx,*captureShared.ring());

            if (!captureReady && captureShared.activateForConsumer(kCapturePrefillFrames)) {
                captureReady=true;
                std::cout << "capture prefill ready: CoreAudio consumer detected; live capture enabled\n";
            }

            const CFAbsoluteTime now=CFAbsoluteTimeGetCurrent();
            if (now-lastStatus>=2.0) {
                const auto w=input.ring()->writeFrame.load(std::memory_order_acquire);
                const auto captureFrames=captureShared.ring()->decodedFrames.load(std::memory_order_acquire);
                const auto& txStats=streamer.stats();
                const auto& rxStats=capturePump.stats();
                std::cout << "HAL out=" << w << " (delta " << (w-lastWrite) << ")"
                          << " shared=" << macfw::hal::availableFrames(*input.ring())
                          << " pcm=" << pcm.availableFrames()
                          << " out-drops=" << input.ring()->droppedFrames.load()
                          << " tx-late=" << txStats.lateCyclePolls
                          << " tx-silence=" << txStats.framesSilenced
                          << " | capture=" << captureFrames << " (delta " << (captureFrames-lastCaptureFrames) << ")"
                          << " active=" << captureShared.ring()->active.load(std::memory_order_acquire)
                          << " queued=" << macfw::hal::capture::availableFrames(*captureShared.ring())
                          << " in-drops=" << captureShared.ring()->droppedFrames.load(std::memory_order_acquire)
                          << " malformed=" << captureShared.ring()->malformedPackets.load(std::memory_order_acquire)
                          << " invalid=" << captureShared.ring()->invalidLabels.load(std::memory_order_acquire)
                          << " chunks=" << rxStats.completedChunks
                          << " dbc-gap=" << rxStats.dbcDiscontinuities
                          << " ts-back=" << rxStats.timestampRegressions
                          << " reorder=" << rxStats.reorderedPackets
                          << " stale=" << rxStats.stalePackets
                          << " hal-read=" << captureShared.ring()->halReadCalls.load(std::memory_order_acquire)
                          << '\n';
                lastWrite=w;
                lastCaptureFrames=captureFrames;
                lastStatus=now;
            }
        }
        std::cout << "stop requested; restoring ISO/CMP resources\n";
    }
    ok=true;
cleanup:
    if (playStart) (*playback.nativeChannel())->Stop(playback.nativeChannel());
    if (capStart) (*capture.nativeChannel())->Stop(capture.nativeChannel());
    if (ipConn) macfw::cmp::restore(device,macfw::cmp::kIpcr0AddressLo,ipcr0);
    if (opConn) macfw::cmp::restore(device,macfw::cmp::kOpcr0AddressLo,opcr0);
    playback.release(); capture.release(); fcp.reset();
    if (notif) (*native)->TurnOffNotification(native);
    if (iso) (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (cb) (*native)->RemoveCallbackDispatcherFromRunLoop(native);
    return ok;
}
} // namespace

int halbridge44100_inner_main() {
    gStopRequested = 0;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::cout << "macfw halbridge44100 — native 44.1 kHz full-duplex CoreAudio HAL to FW410 transport\n";
    return run()?0:1;
}
