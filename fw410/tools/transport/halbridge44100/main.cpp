#include "macfw/amdtp_pcm_stream.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/pcm_ring_buffer.h"
#include "macfw_hal_shm.h"
#include "../capture_shared.h"
#include "../full_duplex_shared.h"
#include "../full_duplex_engine_setup.h"
#include "../full_duplex_lifecycle.h"
#include "../full_duplex_runtime.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>

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
    FullDuplexEngineSetup setup;
    if (!setup.prepare(44100, kCycleLead,
                       "shared HAL ring not available; install/restart the HAL plug-in first",
                       "HAL device must be set to 44100 Hz for this milestone"))
        return false;

    macfw::transport::CaptureReceivePump capturePump(0x01, true);
    macfw::PcmRingBuffer pcm(kPcmCapacityFrames,kPcmChannels);
    auto rx=macfw::AmdtpReceiveRing::create(setup.device,kCaptureSlots,kCaptureMaxPacket);
    auto tx=macfw::AmdtpTransmitRing::createSilence44100(setup.device,setup.firstCycle,kPlaybackSlots);
    if (!pcm.valid() || !rx || !tx) return false;

    macfw::AmdtpPcmStream44100 streamer(tx,pcm,setup.initialCycle,setup.firstCycle,kHalfPackets);
    if (!streamer.valid() || !streamer.prime()) return false;

    FireWireDuplexLifecycle lifecycle;
    if (!lifecycle.prepare(setup.device, rx, tx.nativeLocalPort(),
                           kCaptureMaxPacket, kPlaybackMaxPacket))
        return false;

    FcpRateReassertion fcp;
    bool ok=false;
    if (!lifecycle.addCallbackDispatcher() || !fcp.arm(setup.device)) goto cleanup;
    if (!lifecycle.startIsoch()) goto cleanup;

    {
        UInt32 nowCt=0; if ((*setup.native)->GetCycleTime(setup.native,&nowCt)!=kIOReturnSuccess) goto cleanup;
        const UInt32 now=cycleCount(nowCt), forward=(setup.firstCycle+kCyclesPerSecond-now)%kCyclesPerSecond;
        if (forward>4096u) goto cleanup;
        const double wait=static_cast<double>(forward)/kCyclesPerSecond+0.020;
        std::cout << "duplex ISO started; waiting " << std::fixed << std::setprecision(3) << wait
                  << " s before 44.1 reassert\n" << std::defaultfloat;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,wait,false);
    }
    if (!fcp.reassert44100()) goto cleanup;

    {
        FullDuplexRuntimeConfig runtimeConfig;
        runtimeConfig.rateLabel = "44.1";
        runtimeConfig.prefillMilliseconds = 93;
        runtimeConfig.runLoopSliceSeconds = 0.001;

        runFullDuplexServiceLoop(
            gStopRequested, setup.native, setup.input, pcm, rx, setup.captureShared, capturePump,
            streamer, runtimeConfig,
            [&](std::vector<float>& audio, std::vector<std::int32_t>& mapped) {
                pumpPlayback(*setup.input.ring(), pcm, audio, mapped);
            });

        std::cout << "stop requested; restoring ISO/CMP resources\n";
    }
    ok=true;

cleanup:
    lifecycle.stopIsochAndRestoreCmp();
    fcp.reset();
    lifecycle.removeDispatchers();
    lifecycle.stop();
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
