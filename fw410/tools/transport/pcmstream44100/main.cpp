#include "macfw/amdtp_pcm_stream.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"
#include "macfw/pcm_ring_buffer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr UInt32 kCaptureMaxPacket = 168;
constexpr UInt32 kPlaybackMaxPacket = 360;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kPlaybackSlots = 640;
constexpr std::size_t kHalfPackets = 320;
constexpr std::size_t kPcmChannels = 10;
constexpr std::size_t kPcmCapacityFrames = 16384;
constexpr UInt32 kCycleLead = 2048;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr double kRunSeconds = 6.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kAmplitude = 131072.0;
constexpr std::uint64_t kToneSegmentFrames = 22050;

constexpr UInt16 kFcpAddressHi = 0xffff;
constexpr UInt32 kFcpCommandLo = 0xf0000b00;
constexpr UInt32 kFcpResponseLo = 0xf0000d00;
constexpr UInt32 kFcpResponseSize = 0x200;
constexpr double kFcpTimeoutSeconds = 1.0;

UInt32 cycleCount(UInt32 cycleTime) { return (cycleTime >> 12) & 0x1fffu; }

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
        notificationOn_ = true;
        return true;
    }
    bool reassert44100() {
        const bool out = setRate(0x18);
        const bool in = setRate(0x19);
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
    static UInt32 responseHandler(IOFireWireLibPseudoAddressSpaceRef space,
                                  FWClientCommandID commandID, UInt32 packetLen,
                                  void* packet, UInt16 srcNodeID, UInt32, UInt32,
                                  void* refCon) {
        auto* ctx = static_cast<ResponseContext*>(refCon);
        if (ctx && packet && srcNodeID == ctx->expectedNode) {
            ctx->length = std::min<UInt32>(packetLen, ctx->bytes.size());
            std::memcpy(ctx->bytes.data(), packet, ctx->length);
            ctx->received = true;
        }
        (*space)->ClientCommandIsComplete(space, commandID, kIOReturnSuccess);
        return kIOReturnSuccess;
    }
    bool transaction(const UInt8* cmd, UInt32 len) {
        response_.received = false; response_.length = 0; response_.bytes.fill(0);
        FWAddress address{}; address.nodeID = node_; address.addressHi = kFcpAddressHi; address.addressLo = kFcpCommandLo;
        UInt32 size = len;
        if ((*native_)->Write(native_, 0, &address, cmd, &size, true, generation_) != kIOReturnSuccess) return false;
        const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kFcpTimeoutSeconds;
        while (!response_.received && CFAbsoluteTimeGetCurrent() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        return response_.received;
    }
    bool setRate(UInt8 opcode) {
        const UInt8 cmd[8] = {0x00, 0xff, opcode, 0x00, 0x90, 0x01, 0xff, 0xff};
        if (!transaction(cmd, sizeof(cmd))) return false;
        const UInt8 r = response_.length ? response_.bytes[0] : 0;
        const bool accepted = r == 0x09 || r == 0x0c || r == 0x0d || r == 0x0f;
        return response_.length >= 8 && accepted && response_.bytes[1] == 0xff &&
               response_.bytes[2] == opcode && response_.bytes[3] == 0x00 &&
               response_.bytes[4] == 0x90 && (response_.bytes[5] & 0x07) == 0x01;
    }
    IOFireWireLibDeviceRef native_ = nullptr;
    IOFireWireLibPseudoAddressSpaceRef responseSpace_ = nullptr;
    ResponseContext response_{}; UInt32 generation_ = 0; UInt16 node_ = 0; bool notificationOn_ = false;
};

struct Producer {
    std::uint64_t nextFrame = 0;
    std::size_t fill(macfw::PcmRingBuffer& ring) {
        const std::size_t free = ring.freeFrames();
        if (!free) return 0;
        const std::size_t chunk = std::min<std::size_t>(free, 1024);
        std::vector<std::int32_t> pcm(chunk * kPcmChannels, 0);
        for (std::size_t i = 0; i < chunk; ++i) {
            const std::uint64_t frame = nextFrame + i;
            const bool high = ((frame / kToneSegmentFrames) & 1u) != 0;
            const double hz = high ? 880.0 : 440.0;
            const double phase = 2.0 * kPi * hz * static_cast<double>(frame) / 44100.0;
            pcm[i * kPcmChannels + 1] = static_cast<std::int32_t>(std::sin(phase) * kAmplitude);
        }
        const std::size_t written = ring.write(pcm.data(), chunk);
        nextFrame += written;
        return written;
    }
};

void fillAhead(macfw::PcmRingBuffer& ring, Producer& producer) {
    while (ring.freeFrames() >= 256) if (!producer.fill(ring)) break;
}

void producerLoop(macfw::PcmRingBuffer& ring, Producer& producer,
                  std::atomic<bool>& stop, std::atomic<std::uint64_t>& fills) {
    while (!stop.load(std::memory_order_acquire)) {
        if (producer.fill(ring)) fills.fetch_add(1, std::memory_order_relaxed);
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void dumpCapture(const macfw::AmdtpReceiveRing& ring) {
    std::size_t touched = 0, data = 0, fdf = 0;
    for (std::size_t i = 0; i < ring.packetCount(); ++i) {
        const auto& slot = ring.slot(i); if (!slot.touched()) continue; ++touched;
        const auto p = slot.packet(); if (p.length > 8) ++data; if (p.hasCip() && p.cip().fdf == 0x01) ++fdf;
    }
    std::cout << "capture summary:\n"
              << "    touched slots:      " << touched << " / " << ring.packetCount() << '\n'
              << "    data-bearing slots: " << data << '\n'
              << "    FDF=0x01 slots:     " << fdf << '\n';
}

bool run(bool execute) {
    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) { std::cout << "No operational FW 410 unit found.\n"; return false; }
    std::cout << "FW410 operational unit:\n"
              << "    generation: " << device.generation() << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID() << std::dec << '\n';
    if (device.open() != kIOReturnSuccess) return false;

    std::uint32_t opcr0 = 0, ipcr0 = 0;
    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) return false;
    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) || !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n"; return false;
    }

    auto native = device.nativeHandle(); UInt32 cycleTime = 0;
    if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) return false;
    const UInt32 initialCycle = cycleCount(cycleTime);
    const UInt32 firstCycle = (initialCycle + kCycleLead) % kCyclesPerSecond;

    std::cout << "native 44.1 live scheduler isolation test:\n"
              << "    producer:           generated 440/880 Hz tone, no CoreAudio\n"
              << "    PCM FIFO:           " << kPcmCapacityFrames << " frames\n"
              << "    TX ring:            640 cycles / 320-cycle halves\n"
              << "    scheduler:          AmdtpPcmStream44100\n"
              << "    output:             Analog Out 1\n"
              << "    observation window: " << kRunSeconds << " s\n";
    if (!execute) return true;

    macfw::PcmRingBuffer pcm(kPcmCapacityFrames, kPcmChannels); Producer producer; fillAhead(pcm, producer);
    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = macfw::AmdtpTransmitRing::createSilence44100(device, firstCycle, kPlaybackSlots);
    auto capture = macfw::IsochAllocation::create(device, macfw::IsochAllocation::Direction::DeviceToHost, kCaptureMaxPacket);
    auto playback = macfw::IsochAllocation::create(device, macfw::IsochAllocation::Direction::HostToDevice, kPlaybackMaxPacket);
    if (!rx || !tx || !capture || !playback || !pcm.valid()) return false;

    macfw::AmdtpPcmStream44100 streamer(tx, pcm, initialCycle, firstCycle, kHalfPackets);
    if (!streamer.valid() || !streamer.prime()) { std::cout << "TX stream scheduler prime failed\n"; return false; }
    fillAhead(pcm, producer);

    (*capture.nativeChannel())->AddListener(capture.nativeChannel(), reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
    if (playback.bindHostToDeviceTalkerFirst(tx.nativeLocalPort()) != kIOReturnSuccess) return false;

    FcpRateReassertion fcp;
    bool cb=false, iso=false, notif=false, capStart=false, playStart=false, opConn=false, ipConn=false, ok=false;
    std::atomic<bool> stop{false}; std::atomic<std::uint64_t> fills{0}; std::thread producerThread;
    if ((*native)->AddCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) cb=true;
    if (!cb || !fcp.arm(device)) { std::cout << "FCP response setup failed\n"; goto cleanup; }
    if ((*native)->AddIsochCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) iso=true;
    if ((*native)->TurnOnNotification(native)) notif=true;
    if (capture.allocate()!=kIOReturnSuccess || playback.allocate()!=kIOReturnSuccess) goto cleanup;
    std::cout << "ISO resources:\n    capture channel:  " << capture.channel() << "\n    playback channel: " << playback.channel() << '\n';
    if (macfw::cmp::connectOpcr0(device,opcr0,capture.channel(),capture.speed())!=kIOReturnSuccess) goto cleanup; opConn=true;
    if (macfw::cmp::connectIpcr0(device,ipcr0,playback.channel())!=kIOReturnSuccess) goto cleanup; ipConn=true;
    if ((*playback.nativeChannel())->Start(playback.nativeChannel())!=kIOReturnSuccess) goto cleanup; playStart=true;
    if ((*capture.nativeChannel())->Start(capture.nativeChannel())!=kIOReturnSuccess) goto cleanup; capStart=true;

    {
        UInt32 ct=0; if ((*native)->GetCycleTime(native,&ct)!=kIOReturnSuccess) goto cleanup;
        const UInt32 now=cycleCount(ct); const UInt32 forward=(firstCycle+kCyclesPerSecond-now)%kCyclesPerSecond;
        if (forward>4096u) goto cleanup;
        const double wait=static_cast<double>(forward)/kCyclesPerSecond+0.020;
        std::cout << "duplex ISO: started\nwaiting " << std::fixed << std::setprecision(3) << wait
                  << " s so the reassert occurs ~20 ms after TX begins\n" << std::defaultfloat;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,wait,false);
    }
    if (!fcp.reassert44100()) { std::cout << "status: FAIL - reassert rejected\n"; goto cleanup; }

    producerThread=std::thread(producerLoop,std::ref(pcm),std::ref(producer),std::ref(stop),std::ref(fills));
    std::cout << "listen for a clean alternating 440/880 Hz tone on Analog Out 1\n";
    {
        const CFAbsoluteTime deadline=CFAbsoluteTimeGetCurrent()+kRunSeconds;
        while (CFAbsoluteTimeGetCurrent()<deadline) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.002,false);
            UInt32 ct=0; if ((*native)->GetCycleTime(native,&ct)==kIOReturnSuccess) streamer.service(cycleCount(ct));
        }
    }
    stop.store(true,std::memory_order_release); if (producerThread.joinable()) producerThread.join();
    dumpCapture(rx);
    {
        const auto& s=streamer.stats();
        std::cout << "streaming statistics:\n"
                  << "    PCM produced frames:  " << pcm.producedFrames() << '\n'
                  << "    PCM consumed frames:  " << pcm.consumedFrames() << '\n'
                  << "    PCM underrun frames:  " << pcm.underrunFrames() << '\n'
                  << "    TX halves refilled:   " << s.halvesRefilled << '\n'
                  << "    TX data packets:      " << s.dataPacketsRefilled << '\n'
                  << "    refill silence frames:" << s.framesSilenced << '\n'
                  << "    late cycle polls:     " << s.lateCyclePolls << '\n'
                  << "    producer fill calls:  " << fills.load() << '\n';
    }
    ok=true;
cleanup:
    stop.store(true,std::memory_order_release); if (producerThread.joinable()) producerThread.join();
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

int pcmstream44100_inner_main(int argc, char** argv) {
    bool execute=false;
    for (int i=1;i<argc;++i) { const std::string arg=argv[i]; if (arg=="--execute") execute=true; else { std::cerr << "usage: ./pcmstream44100 [--execute]\n"; return 64; } }
    std::cout << "macfw pcmstream44100 — native 44.1 live scheduler isolation test\n\n";
    return run(execute)?0:1;
}
