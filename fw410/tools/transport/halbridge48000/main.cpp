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
#include <iostream>
#include <pthread.h>

namespace {
using namespace macfw::transport::duplex;
constexpr UInt32 kCycleLead = 256;

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
    if (!setup.prepare(48000, kCycleLead,
                       "shared HAL playback ring not available; install/restart the HAL plug-in first",
                       "HAL device must be set to 48000 Hz for this engine"))
        return false;

    macfw::transport::CaptureReceivePump capturePump(0x02);
    macfw::PcmRingBuffer pcm(kPcmCapacityFrames, kPcmChannels);
    auto rx = macfw::AmdtpReceiveRing::create(setup.device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = macfw::AmdtpTransmitRing::createSilence48k(setup.device, setup.firstCycle, kPlaybackSlots);
    if (!pcm.valid() || !rx || !tx) return false;

    macfw::AmdtpPcmStream48k streamer(tx, pcm, setup.initialCycle, setup.firstCycle, kHalfPackets);
    if (!streamer.valid() || !streamer.prime()) return false;

    FireWireDuplexLifecycle lifecycle;
    if (!lifecycle.prepare(setup.device, rx, tx.nativeLocalPort(),
                           kCaptureMaxPacket, kPlaybackMaxPacket))
        return false;
    if (!lifecycle.addCallbackDispatcher()) return false;

    Fw410FcpControl fcp;
    Fw410ControlServer control;
    macfw::transport::CaptureMeterServer meterServer;
    if (!fcp.arm(setup.device)) {
        std::cerr << "warning: FW410 FCP control unavailable; audio will continue\n";
    } else if (!control.start(fcp)) {
        std::cerr << "warning: FW410 control socket unavailable; audio will continue\n";
    }
    if (!meterServer.start(capturePump))
        std::cerr << "warning: FW410 meter socket unavailable; audio will continue\n";

    if (!lifecycle.startIsoch()) return false;

    requestInteractiveQos();
    macfw::transport::signalEngineReady();

    FullDuplexRuntimeConfig runtimeConfig;
    runtimeConfig.rateLabel = "48";
    runtimeConfig.prefillMilliseconds = 85;
    runtimeConfig.runLoopSliceSeconds = 0.00025;
    runtimeConfig.printIsoStarted = true;
    runtimeConfig.printTransportGeometry = true;
    runtimeConfig.expectedGeneration = setup.device.generation();

    const bool runtimeOk = runFullDuplexServiceLoop(
        gStopRequested, setup.native, setup.input, pcm, rx, setup.captureShared, capturePump,
        streamer, runtimeConfig,
        [&](std::vector<float>& audio, std::vector<std::int32_t>& mapped) {
            control.service();
            meterServer.service();
            drainPlayback(*setup.input.ring(), pcm, audio, mapped);
        });

    std::cout << "stop requested; restoring ISO/CMP resources\n";
    meterServer.reset();
    control.reset();
    fcp.reset();
    lifecycle.stop();
    return runtimeOk;
}
} // namespace

int halbridge48000_inner_main() {
    gStopRequested = 0;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::cout << "macfw halbridge48000 — native 48 kHz full-duplex CoreAudio HAL to FW410 transport\n";
    return run() ? 0 : 1;
}
