#include "macfw/amdtp_pcm_stream.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/amdtp_transmit_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"
#include "macfw/pcm_ring_buffer.h"

#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <AvailabilityMacros.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
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
constexpr double kRequiredRate = 44100.0;
constexpr double kRunSeconds = 8.0;
constexpr UInt32 kMaxAudioFrames = 4096;

constexpr UInt16 kFcpAddressHi = 0xffff;
constexpr UInt32 kFcpCommandLo = 0xf0000b00;
constexpr UInt32 kFcpResponseLo = 0xf0000d00;
constexpr UInt32 kFcpResponseSize = 0x200;
constexpr double kFcpTimeoutSeconds = 1.0;

#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 120000
constexpr AudioObjectPropertyElement kMainElement = kAudioObjectPropertyElementMain;
#else
constexpr AudioObjectPropertyElement kMainElement = kAudioObjectPropertyElementMaster;
#endif

UInt32 cycleCount(UInt32 cycleTime) { return (cycleTime >> 12) & 0x1fffu; }

std::string osStatusText(OSStatus s) {
    char text[8] = {};
    const UInt32 u = static_cast<UInt32>(s);
    text[0] = static_cast<char>((u >> 24) & 0xff);
    text[1] = static_cast<char>((u >> 16) & 0xff);
    text[2] = static_cast<char>((u >> 8) & 0xff);
    text[3] = static_cast<char>(u & 0xff);
    const bool printable = text[0] >= 32 && text[0] < 127 && text[1] >= 32 && text[1] < 127 &&
                           text[2] >= 32 && text[2] < 127 && text[3] >= 32 && text[3] < 127;
    return printable ? std::string("'") + std::string(text, 4) + "'" : std::to_string(s);
}

class FcpRateReassertion {
public:
    ~FcpRateReassertion() { reset(); }

    bool arm(macfw::FireWireDevice& device) {
        native_ = device.nativeHandle();
        generation_ = device.generation();
        node_ = device.nodeID();
        response_.expectedNode = node_;
        responseSpace_ = (*native_)->CreateInitialUnitsPseudoAddressSpace(
            native_, kFcpResponseLo, kFcpResponseSize, &response_, 1024, nullptr,
            kFWAddressSpaceNoReadAccess | kFWAddressSpaceShareIfExists,
            CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
        if (!responseSpace_) return false;
        (*responseSpace_)->SetWriteHandler(responseSpace_, responseHandler);
        if (!(*responseSpace_)->TurnOnNotification(responseSpace_)) {
            reset();
            return false;
        }
        notificationOn_ = true;
        return true;
    }

    bool reassert44100() {
        const bool output = setRate(0x18);
        const bool input = setRate(0x19);
        std::cout << "post-start AV/C reassert:\n"
                  << "    OUTPUT plug 0 -> 44100: " << (output ? "accepted" : "failed") << '\n'
                  << "    INPUT plug 0  -> 44100: " << (input ? "accepted" : "failed") << '\n';
        return output && input;
    }

    void reset() {
        if (responseSpace_) {
            if (notificationOn_) (*responseSpace_)->TurnOffNotification(responseSpace_);
            (*responseSpace_)->Release(responseSpace_);
        }
        responseSpace_ = nullptr;
        notificationOn_ = false;
        native_ = nullptr;
    }

private:
    struct ResponseContext {
        UInt16 expectedNode = 0;
        bool received = false;
        UInt32 length = 0;
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
        response_.received = false;
        response_.length = 0;
        response_.bytes.fill(0);
        FWAddress address{};
        address.nodeID = node_;
        address.addressHi = kFcpAddressHi;
        address.addressLo = kFcpCommandLo;
        UInt32 size = len;
        if ((*native_)->Write(native_, 0, &address, cmd, &size, true, generation_) != kIOReturnSuccess)
            return false;
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
    ResponseContext response_{};
    UInt32 generation_ = 0;
    UInt16 node_ = 0;
    bool notificationOn_ = false;
};

struct CoreAudioInput {
    AudioUnit unit = nullptr;
    AudioDeviceID device = kAudioObjectUnknown;
    macfw::PcmRingBuffer* ring = nullptr;
    double nativeRate = 0.0;
    std::vector<Float32> mono;
    std::vector<std::int32_t> mapped;
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> renderedFrames{0};
    std::atomic<std::uint64_t> writtenFrames{0};
    std::atomic<std::uint64_t> droppedFrames{0};
    std::atomic<std::uint64_t> renderErrors{0};
    std::atomic<std::int32_t> firstRenderError{0};

    ~CoreAudioInput() { stop(); }

    static OSStatus inputCallback(void* refCon, AudioUnitRenderActionFlags* flags,
                                  const AudioTimeStamp* ts, UInt32, UInt32 frames,
                                  AudioBufferList*) {
        auto* self = static_cast<CoreAudioInput*>(refCon);
        ++self->callbacks;
        if (!self->ring || !self->unit || frames == 0) return noErr;
        if (frames > kMaxAudioFrames) {
            self->droppedFrames += frames;
            return noErr;
        }

        AudioBufferList abl{};
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 1;
        abl.mBuffers[0].mDataByteSize = frames * sizeof(Float32);
        abl.mBuffers[0].mData = self->mono.data();
        const OSStatus err = AudioUnitRender(self->unit, flags, ts, 1, frames, &abl);
        if (err != noErr) {
            ++self->renderErrors;
            std::int32_t expected = 0;
            self->firstRenderError.compare_exchange_strong(expected, static_cast<std::int32_t>(err));
            return err;
        }
        self->renderedFrames += frames;

        for (UInt32 i = 0; i < frames; ++i) {
            const double x = std::max(-1.0, std::min(1.0, static_cast<double>(self->mono[i])));
            const auto s24 = static_cast<std::int32_t>(x * 8388607.0);
            const std::size_t base = static_cast<std::size_t>(i) * kPcmChannels;
            std::fill_n(self->mapped.data() + base, kPcmChannels, 0);
            self->mapped[base + 1] = s24;
            self->mapped[base + 6] = s24;
        }

        const std::size_t written = self->ring->write(self->mapped.data(), frames);
        self->writtenFrames += written;
        self->droppedFrames += (frames - written);
        return noErr;
    }

    bool configure(macfw::PcmRingBuffer& target) {
        ring = &target;
        mono.assign(kMaxAudioFrames, 0.0f);
        mapped.assign(static_cast<std::size_t>(kMaxAudioFrames) * kPcmChannels, 0);

        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_HALOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) return false;
        OSStatus err = AudioComponentInstanceNew(comp, &unit);
        if (err != noErr) return false;

        UInt32 one = 1, zero = 0;
        if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
                                 1, &one, sizeof(one)) != noErr) return false;
        if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                                 0, &zero, sizeof(zero)) != noErr) return false;

        AudioObjectPropertyAddress defaultAddr{kAudioHardwarePropertyDefaultInputDevice,
                                               kAudioObjectPropertyScopeGlobal, kMainElement};
        UInt32 size = sizeof(device);
        err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultAddr, 0, nullptr, &size, &device);
        if (err != noErr || device == kAudioObjectUnknown) return false;
        if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                 kAudioUnitScope_Global, 0, &device, sizeof(device)) != noErr) return false;

        AudioObjectPropertyAddress formatAddr{kAudioDevicePropertyStreamFormat,
                                              kAudioDevicePropertyScopeInput, kMainElement};
        AudioStreamBasicDescription deviceAsbd{};
        size = sizeof(deviceAsbd);
        err = AudioObjectGetPropertyData(device, &formatAddr, 0, nullptr, &size, &deviceAsbd);
        if (err != noErr || deviceAsbd.mSampleRate <= 0.0) return false;
        nativeRate = deviceAsbd.mSampleRate;
        if (nativeRate < 44099.0 || nativeRate > 44101.0) {
            std::cerr << "native 44.1 bridge requires a 44100 Hz CoreAudio input; got "
                      << nativeRate << " Hz\n";
            return false;
        }

        AudioStreamBasicDescription asbd{};
        asbd.mSampleRate = nativeRate;
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
        asbd.mBytesPerPacket = sizeof(Float32);
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerFrame = sizeof(Float32);
        asbd.mChannelsPerFrame = 1;
        asbd.mBitsPerChannel = 32;
        if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                 1, &asbd, sizeof(asbd)) != noErr) return false;

        UInt32 maxFrames = kMaxAudioFrames;
        AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
                             kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames));
        AURenderCallbackStruct cb{};
        cb.inputProc = &CoreAudioInput::inputCallback;
        cb.inputProcRefCon = this;
        if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback,
                                 kAudioUnitScope_Global, 0, &cb, sizeof(cb)) != noErr) return false;
        if (AudioUnitInitialize(unit) != noErr) return false;

        std::cout << "CoreAudio input device:\n"
                  << "    device id:          " << device << '\n'
                  << "    native rate:        " << nativeRate << " Hz\n"
                  << "    AUHAL client:       44100 Hz mono Float32\n"
                  << "    bridge SRC:         none (native 44.1 -> native 44.1)\n";
        return true;
    }

    bool start() { return AudioOutputUnitStart(unit) == noErr; }
    void stop() {
        if (!unit) return;
        AudioOutputUnitStop(unit);
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        unit = nullptr;
    }
};

void dumpCapture(const macfw::AmdtpReceiveRing& ring) {
    std::size_t touched = 0, dataBearing = 0, fdf = 0;
    for (std::size_t i = 0; i < ring.packetCount(); ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched()) continue;
        ++touched;
        const auto p = slot.packet();
        if (p.length > 8) ++dataBearing;
        if (p.hasCip() && p.cip().fdf == 0x01) ++fdf;
    }
    std::cout << "capture summary:\n"
              << "    touched slots:      " << touched << " / " << ring.packetCount() << '\n'
              << "    data-bearing slots: " << dataBearing << '\n'
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
    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n";
        return false;
    }

    auto native = device.nativeHandle();
    UInt32 cycleTime = 0;
    if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) return false;
    const UInt32 initialCycle = cycleCount(cycleTime);
    const UInt32 firstCycle = (initialCycle + kCycleLead) % kCyclesPerSecond;

    std::cout << "native 44.1 CoreAudio bridge:\n"
              << "    FW410 clock domain: 44100 Hz\n"
              << "    PCM FIFO:           " << kPcmCapacityFrames << " frames\n"
              << "    TX ring:            640 cycles / two 320-cycle halves\n"
              << "    scheduler:          AmdtpPcmStream44100\n"
              << "    bridge SRC:         none\n"
              << "    startup quirk:      post-start AV/C 44100 reassert before first TX cycle\n"
              << "    observation window: " << kRunSeconds << " s\n";
    if (!execute) return true;

    macfw::PcmRingBuffer pcm(kPcmCapacityFrames, kPcmChannels);
    CoreAudioInput input;
    if (!pcm.valid() || !input.configure(pcm) || !input.start()) return false;
    const CFAbsoluteTime prefillDeadline = CFAbsoluteTimeGetCurrent() + 1.0;
    while (pcm.availableFrames() < 2048 && CFAbsoluteTimeGetCurrent() < prefillDeadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, false);

    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = macfw::AmdtpTransmitRing::createSilence44100(device, firstCycle, kPlaybackSlots);
    auto capture = macfw::IsochAllocation::create(device,
        macfw::IsochAllocation::Direction::DeviceToHost, kCaptureMaxPacket);
    auto playback = macfw::IsochAllocation::create(device,
        macfw::IsochAllocation::Direction::HostToDevice, kPlaybackMaxPacket);
    if (!rx || !tx || !capture || !playback) return false;

    macfw::AmdtpPcmStream44100 streamer(tx, pcm, initialCycle, firstCycle, kHalfPackets);
    if (!streamer.valid() || !streamer.prime()) {
        std::cout << "TX stream scheduler prime failed\n";
        return false;
    }

    (*capture.nativeChannel())->AddListener(capture.nativeChannel(),
        reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
    if (playback.bindHostToDeviceTalkerFirst(tx.nativeLocalPort()) != kIOReturnSuccess) return false;

    FcpRateReassertion fcp;
    bool cb = false, iso = false, notif = false, capStart = false, playStart = false;
    bool opConn = false, ipConn = false, ok = false;
    if ((*native)->AddCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) cb = true;
    if (!cb || !fcp.arm(device)) { std::cout << "FCP response setup failed\n"; goto cleanup; }
    if ((*native)->AddIsochCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) iso = true;
    if ((*native)->TurnOnNotification(native)) notif = true;

    if (capture.allocate() != kIOReturnSuccess || playback.allocate() != kIOReturnSuccess) goto cleanup;
    std::cout << "ISO resources:\n"
              << "    capture channel:  " << capture.channel() << '\n'
              << "    playback channel: " << playback.channel() << '\n';
    if (macfw::cmp::connectOpcr0(device, opcr0, capture.channel(), capture.speed()) != kIOReturnSuccess) goto cleanup;
    opConn = true;
    if (macfw::cmp::connectIpcr0(device, ipcr0, playback.channel()) != kIOReturnSuccess) goto cleanup;
    ipConn = true;
    if ((*playback.nativeChannel())->Start(playback.nativeChannel()) != kIOReturnSuccess) goto cleanup;
    playStart = true;
    if ((*capture.nativeChannel())->Start(capture.nativeChannel()) != kIOReturnSuccess) goto cleanup;
    capStart = true;

    // Linux snd-bebob performs the M-Audio rate SET after the AMDTP domain has
    // started. Reassert immediately while the first scheduled TX cycle is still
    // ~256 ms ahead; this tests whether NODATA warm-up was incidental or whether
    // the essential requirement is simply post-start rate control.
    if (!fcp.reassert44100()) {
        std::cout << "status: FAIL - post-start 44100 reassertion rejected\n";
        goto cleanup;
    }

    std::cout << "bridge started: speak/play into the Mac default input; listen on FW410 Out 1/2\n";
    {
        const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kRunSeconds;
        while (CFAbsoluteTimeGetCurrent() < deadline) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, false);
            UInt32 ct = 0;
            if ((*native)->GetCycleTime(native, &ct) == kIOReturnSuccess)
                streamer.service(cycleCount(ct));
        }
    }

    dumpCapture(rx);
    {
        const auto& s = streamer.stats();
        const auto firstError = static_cast<OSStatus>(input.firstRenderError.load());
        std::cout << "bridge statistics:\n"
                  << "    CoreAudio callbacks:   " << input.callbacks.load() << '\n'
                  << "    CoreAudio input frames:" << input.renderedFrames.load() << '\n'
                  << "    PCM written frames:    " << input.writtenFrames.load() << '\n'
                  << "    PCM dropped frames:    " << input.droppedFrames.load() << '\n'
                  << "    CoreAudio errors:      " << input.renderErrors.load() << '\n'
                  << "    first render error:    " << (firstError == noErr ? "none" : osStatusText(firstError)) << '\n'
                  << "    PCM consumed frames:   " << pcm.consumedFrames() << '\n'
                  << "    PCM underrun frames:   " << pcm.underrunFrames() << '\n'
                  << "    TX halves refilled:    " << s.halvesRefilled << '\n'
                  << "    TX data packets refill:" << s.dataPacketsRefilled << '\n'
                  << "    late cycle polls:      " << s.lateCyclePolls << '\n';
    }
    ok = true;

cleanup:
    input.stop();
    if (playStart) (*playback.nativeChannel())->Stop(playback.nativeChannel());
    if (capStart) (*capture.nativeChannel())->Stop(capture.nativeChannel());
    if (ipConn) macfw::cmp::restore(device, macfw::cmp::kIpcr0AddressLo, ipcr0);
    if (opConn) macfw::cmp::restore(device, macfw::cmp::kOpcr0AddressLo, opcr0);
    playback.release();
    capture.release();
    fcp.reset();
    if (notif) (*native)->TurnOffNotification(native);
    if (iso) (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (cb) (*native)->RemoveCallbackDispatcherFromRunLoop(native);
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else { std::cerr << "usage: ./coreaudiobridge44100 [--execute]\n"; return 64; }
    }
    std::cout << "macfw coreaudiobridge44100 — native 44.1 CoreAudio to FW410 bridge\n\n";
    return run(execute) ? 0 : 1;
}
