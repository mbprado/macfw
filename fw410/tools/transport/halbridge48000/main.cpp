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
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <pthread.h>
#include <thread>
#include <vector>

namespace {
using namespace macfw::transport::duplex;
constexpr UInt32 kCycleLead = 256;

volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }

void requestInteractiveQos(const char* label) {
    const int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (rc == 0)
        std::cout << label << " QoS: user-interactive\n";
    else
        std::cout << label << " QoS: request failed (" << rc << ")\n";
}

std::uint32_t machTicksForNanoseconds(std::uint64_t nanoseconds) {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) return 0;
    const long double ticks = static_cast<long double>(nanoseconds) *
                              static_cast<long double>(timebase.denom) /
                              static_cast<long double>(timebase.numer);
    return static_cast<std::uint32_t>(ticks);
}

bool requestAudioTimeConstraint() {
    constexpr std::uint64_t kPeriodNs = 2000000;
    constexpr std::uint64_t kComputationNs = 500000;
    constexpr std::uint64_t kConstraintNs = 2000000;

    thread_time_constraint_policy_data_t policy{};
    policy.period = machTicksForNanoseconds(kPeriodNs);
    policy.computation = machTicksForNanoseconds(kComputationNs);
    policy.constraint = machTicksForNanoseconds(kConstraintNs);
    policy.preemptible = TRUE;
    if (policy.period == 0 || policy.computation == 0 || policy.constraint == 0) {
        std::cout << "audio service thread time-constraint: unavailable (timebase)\n";
        return false;
    }

    const thread_port_t threadPort = pthread_mach_thread_np(pthread_self());
    const kern_return_t kr = thread_policy_set(
        threadPort,
        THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr == KERN_SUCCESS) {
        std::cout << "audio service thread time-constraint: period=2000 us computation=500 us constraint=2000 us\n";
        return true;
    }

    std::cout << "audio service thread time-constraint: request failed (" << kr
              << "); continuing with QoS only\n";
    return false;
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

    Fw410FcpControl fcp;
    Fw410ControlServer control;
    macfw::transport::CaptureMeterServer meterServer;
    bool ok = false;

    if (!lifecycle.addCallbackDispatcher()) goto cleanup;
    if (!fcp.arm(setup.device)) {
        std::cerr << "warning: FW410 FCP control unavailable; audio will continue\n";
    } else if (!control.start(fcp)) {
        std::cerr << "warning: FW410 control socket unavailable; audio will continue\n";
    }
    if (!meterServer.start(capturePump))
        std::cerr << "warning: FW410 meter socket unavailable; audio will continue\n";

    // Keep 48 kHz isoch callback ownership on the transport thread. Audio
    // service remains isolated on its dedicated Mach-paced real-time worker,
    // but returning the dispatcher to the original run loop restores the
    // proven FireWire teardown/rate-transition lifecycle.
    if (!lifecycle.startIsoch()) goto cleanup;
    std::cout << "isoch callback dispatcher: transport run-loop thread\n";

    requestInteractiveQos("transport thread");
    macfw::transport::signalEngineReady();

    {
        std::atomic<bool> audioFinished{false};
        bool audioOk = false;

        std::thread audioThread([&] {
            requestInteractiveQos("audio service thread");
            requestAudioTimeConstraint();
            std::cout << "audio service: dedicated Mach-paced thread (250 us)\n";

            FullDuplexRuntimeConfig runtimeConfig;
            runtimeConfig.rateLabel = "48";
            runtimeConfig.prefillMilliseconds = 85;
            runtimeConfig.useMachPacing = true;
            runtimeConfig.machPaceNanoseconds = 250000;
            runtimeConfig.printIsoStarted = true;
            runtimeConfig.printTransportGeometry = true;
            runtimeConfig.expectedGeneration = setup.device.generation();

            audioOk = runFullDuplexServiceLoop(
                gStopRequested, setup.native, setup.input, pcm, rx, setup.captureShared, capturePump,
                streamer, runtimeConfig,
                [&](std::vector<float>& audio, std::vector<std::int32_t>& mapped) {
                    meterServer.service();
                    drainPlayback(*setup.input.ring(), pcm, audio, mapped);
                });

            audioFinished.store(true, std::memory_order_release);
        });

        while (!gStopRequested && !audioFinished.load(std::memory_order_acquire)) {
            // This run loop now services only FireWire callbacks and the control
            // socket; all PCM/capture/TX work remains on the real-time worker.
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.00025, false);
            control.service();
        }

        audioThread.join();
        ok = audioOk;
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

int halbridge48000_inner_main() {
    gStopRequested = 0;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::cout << "macfw halbridge48000 — native 48 kHz full-duplex CoreAudio HAL to FW410 transport\n";
    return run() ? 0 : 1;
}
