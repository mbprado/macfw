#include "macfw/amdtp_pcm_stream.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"
#include "macfw/pcm_ring_buffer.h"
#include "macfw_hal_shm.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr UInt32 kCaptureMaxPacket = 168;
constexpr UInt32 kPlaybackMaxPacket = 360;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kPlaybackSlots = 128;
constexpr std::size_t kHalfPackets = 64;
constexpr std::size_t kPcmChannels = 10;
constexpr std::size_t kPcmCapacityFrames = 8192;
constexpr UInt32 kCycleLead = 256;
constexpr UInt32 kCyclesPerSecond = 8000;

volatile std::sig_atomic_t gStopRequested = 0;
void signalHandler(int) { gStopRequested = 1; }
UInt32 cycleCount(UInt32 cycleTime) { return (cycleTime >> 12) & 0x1fffu; }

class SharedInput {
public:
    ~SharedInput() {
        if (ring_) munmap(ring_, sizeof(*ring_));
        if (fd_ >= 0) close(fd_);
    }
    bool open() {
        fd_ = shm_open(macfw::hal::kShmName, O_RDWR, 0);
        if (fd_ < 0) return false;
        void* p = mmap(nullptr, sizeof(macfw::hal::SharedPcmRing), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) { close(fd_); fd_ = -1; return false; }
        ring_ = static_cast<macfw::hal::SharedPcmRing*>(p);
        return macfw::hal::valid(*ring_);
    }
    void discardBacklog() {
        const auto w = ring_->writeFrame.load(std::memory_order_acquire);
        ring_->readFrame.store(w, std::memory_order_release);
    }
    macfw::hal::SharedPcmRing* ring() { return ring_; }
private:
    int fd_ = -1;
    macfw::hal::SharedPcmRing* ring_ = nullptr;
};

std::size_t pumpShared(macfw::hal::SharedPcmRing& shared, macfw::PcmRingBuffer& pcm,
                       std::vector<float>& stereo, std::vector<std::int32_t>& mapped) {
    const std::size_t frames = std::min<std::size_t>({pcm.freeFrames(), macfw::hal::availableFrames(shared), stereo.size()/2});
    if (!frames) return 0;
    const std::size_t got = macfw::hal::read(shared, stereo.data(), frames);
    for (std::size_t i = 0; i < got; ++i) {
        const double l = std::max(-1.0, std::min(1.0, static_cast<double>(stereo[i*2])));
        const double r = std::max(-1.0, std::min(1.0, static_cast<double>(stereo[i*2+1])));
        const auto ls = static_cast<std::int32_t>(l * 8388607.0);
        const auto rs = static_cast<std::int32_t>(r * 8388607.0);
        const std::size_t b = i * kPcmChannels;
        std::fill_n(mapped.data() + b, kPcmChannels, 0);
        mapped[b+1] = ls;
        mapped[b+6] = rs;
    }
    return pcm.write(mapped.data(), got);
}

bool run() {
    SharedInput input;
    if (!input.open()) {
        std::cerr << "shared HAL ring not available; install/restart the HAL plug-in first\n";
        return false;
    }
    if (input.ring()->sampleRate.load(std::memory_order_acquire) != 48000) {
        std::cerr << "HAL device must be set to 48000 Hz for this milestone\n";
        return false;
    }
    input.discardBacklog();

    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) { std::cerr << "No operational FW 410 unit found.\n"; return false; }
    if (device.open() != kIOReturnSuccess) return false;

    std::uint32_t opcr0 = 0, ipcr0 = 0;
    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) return false;
    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cerr << "PCR0 offline or already connected\n";
        return false;
    }

    auto native = device.nativeHandle();
    UInt32 ct = 0;
    if ((*native)->GetCycleTime(native, &ct) != kIOReturnSuccess) return false;
    const UInt32 initialCycle = cycleCount(ct);
    const UInt32 firstCycle = (initialCycle + kCycleLead) % kCyclesPerSecond;

    macfw::PcmRingBuffer pcm(kPcmCapacityFrames, kPcmChannels);
    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = macfw::AmdtpTransmitRing::createSilence48k(device, firstCycle, kPlaybackSlots);
    auto capture = macfw::IsochAllocation::create(device, macfw::IsochAllocation::Direction::DeviceToHost, kCaptureMaxPacket);
    auto playback = macfw::IsochAllocation::create(device, macfw::IsochAllocation::Direction::HostToDevice, kPlaybackMaxPacket);
    if (!pcm.valid() || !rx || !tx || !capture || !playback) return false;

    macfw::AmdtpPcmStream48k streamer(tx, pcm, initialCycle, firstCycle, kHalfPackets);
    if (!streamer.valid() || !streamer.prime()) return false;

    (*capture.nativeChannel())->AddListener(capture.nativeChannel(), reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
    if (playback.bindHostToDeviceTalkerFirst(tx.nativeLocalPort()) != kIOReturnSuccess) return false;

    bool cb=false, iso=false, notif=false, capStart=false, playStart=false, opConn=false, ipConn=false, ok=false;
    if ((*native)->AddCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) cb=true;
    if ((*native)->AddIsochCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) iso=true;
    if ((*native)->TurnOnNotification(native)) notif=true;
    if (capture.allocate()!=kIOReturnSuccess || playback.allocate()!=kIOReturnSuccess) goto cleanup;
    if (macfw::cmp::connectOpcr0(device,opcr0,capture.channel(),capture.speed())!=kIOReturnSuccess) goto cleanup; opConn=true;
    if (macfw::cmp::connectIpcr0(device,ipcr0,playback.channel())!=kIOReturnSuccess) goto cleanup; ipConn=true;
    if ((*playback.nativeChannel())->Start(playback.nativeChannel())!=kIOReturnSuccess) goto cleanup; playStart=true;
    if ((*capture.nativeChannel())->Start(capture.nativeChannel())!=kIOReturnSuccess) goto cleanup; capStart=true;

    {
        std::vector<float> stereo(4096*2,0.0f);
        std::vector<std::int32_t> mapped(4096*kPcmChannels,0);
        std::uint64_t lastWrite=input.ring()->writeFrame.load(std::memory_order_acquire);
        CFAbsoluteTime lastStatus=CFAbsoluteTimeGetCurrent();
        std::cout << "duplex ISO started\n"
                  << "HAL bridge active: play audio to M-Audio FireWire 410 at 48 kHz; Ctrl-C to stop\n";
        while (!gStopRequested) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.001,false);
            pumpShared(*input.ring(),pcm,stereo,mapped);
            UInt32 nowCt=0;
            if ((*native)->GetCycleTime(native,&nowCt)==kIOReturnSuccess) streamer.service(cycleCount(nowCt));
            const CFAbsoluteTime now=CFAbsoluteTimeGetCurrent();
            if (now-lastStatus>=2.0) {
                const auto w=input.ring()->writeFrame.load(std::memory_order_acquire);
                std::cout << "HAL frames=" << w << " (delta " << (w-lastWrite) << ")"
                          << " shared=" << macfw::hal::availableFrames(*input.ring())
                          << " pcm=" << pcm.availableFrames()
                          << " drops=" << input.ring()->droppedFrames.load() << '\n';
                lastWrite=w; lastStatus=now;
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
    playback.release(); capture.release();
    if (notif) (*native)->TurnOffNotification(native);
    if (iso) (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (cb) (*native)->RemoveCallbackDispatcherFromRunLoop(native);
    return ok;
}
} // namespace

int halbridge48000_inner_main() {
    gStopRequested = 0;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::cout << "macfw halbridge48000 — native 48 kHz CoreAudio HAL shared-memory to FW410 transport\n";
    return run() ? 0 : 1;
}
