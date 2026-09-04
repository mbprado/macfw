#include "macfw/amdtp_pcm_stream.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/pcm_ring_buffer.h"
#include "macfw_hal_shm.h"
#include "../capture_shared.h"
#include "../capture_meter_server.h"
#include "../engine_ready.h"
#include "../full_duplex_shared.h"
#include "../full_duplex_engine_setup.h"
#include "../full_duplex_fcp_control.h"
#include "../fw410_control_server.h"
#include "../full_duplex_lifecycle.h"
#include "../full_duplex_runtime.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <pthread.h>

namespace {
using namespace macfw::transport::duplex;
constexpr UInt32 kCycleLead = 2048;

volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }

void requestInteractiveQos() {
    const int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (rc == 0)
        std::cout << "transport thread QoS: user-interactive\n";
    else
        std::cout << "transport thread QoS: request failed (" << rc << ")\n";
}

bool run() {
    FullDuplexEngineSetup setup;
    if (!setup.prepare(44100, kCycleLead,
                       "shared HAL ring not available; install/restart the HAL plug-in first",
                       "HAL device must be set to 44100 Hz for this milestone"))
        return false;

    macfw::transport::CaptureReceivePump capturePump(0x01, true);
    macfw::PcmRingBuffer pcm(kPcmCapacityFrames, kPcmChannels);
    auto rx = macfw::AmdtpReceiveRing::create(setup.device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = macfw::AmdtpTransmitRing::createSilence44100(setup.device, setup.firstCycle, kPlaybackSlots);
    if (!pcm.valid() || !rx || !tx) return false;

    macfw::AmdtpPcmStream44100 streamer(tx, pcm, setup.initialCycle, setup.firstCycle, kHalfPackets);
    if (!streamer.valid() || !streamer.prime()) return false;

    FireWireDuplexLifecycle lifecycle;
    if (!lifecycle.prepare(setup.device, rx, tx.nativeLocalPort(),
                           kCaptureMaxPacket, kPlaybackMaxPacket))
        return false;

    Fw410FcpControl fcp;
    Fw410ControlServer control;
    macfw::transport::CaptureMeterServer meterServer;
    bool ok = false;
    if (!lifecycle.addCallbackDispatcher() || !fcp.arm(setup.device)) goto cleanup;
    if (!control.start(fcp))
        std::cerr << "warning: FW410 control socket unavailable; audio will continue\n";
    if (!meterServer.start(capturePump))
        std::cerr << "warning: FW410 meter socket unavailable; audio will continue\n";
    if (!lifecycle.startIsoch()) goto cleanup;

    requestInteractiveQos();

    {
        UInt32 nowCt = 0;
        if ((*setup.native)->GetCycleTime(setup.native, &nowCt) != kIOReturnSuccess) goto cleanup;
        const UInt32 now = cycleCount(nowCt);
        const UInt32 forward = (setup.firstCycle + kCyclesPerSecond - now) % kCyclesPerSecond;
        if (forward > 4096u) goto cleanup;
        const double wait = static_cast<double>(forward) / kCyclesPerSecond + 0.020;
        std::cout << "duplex ISO started; waiting " << std::fixed << std::setprecision(3) << wait
                  << " s before 44.1 reassert\n" << std::defaultfloat;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, wait, false);
    }

    if (!fcp.reassert44100()) goto cleanup;
    macfw::transport::signalEngineReady();

    {
        FullDuplexRuntimeConfig runtimeConfig;
        runtimeConfig.rateLabel = "44.1";
        runtimeConfig.prefillMilliseconds = 93;
        runtimeConfig.runLoopSliceSeconds = 0.00025;
        runtimeConfig.runLoopReturnAfterSourceHandled = true;
        runtimeConfig.expectedGeneration = setup.device.generation();

        ok = runFullDuplexServiceLoop(
            gStopRequested, setup.native, setup.input, pcm, rx, setup.captureShared, capturePump,
            streamer, runtimeConfig,
            [&](std::vector<float>& audio, std::vector<std::int32_t>& mapped) {
                control.service();
                meterServer.service();
                pumpPlayback(*setup.input.ring(), pcm, audio, mapped);
            });

        std::cout << "stop requested; restoring ISO/CMP resources\n";
    }

cleanup:
    meterServer.reset();
    control.reset();
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
    return run() ? 0 : 1;
}
