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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr UInt32 kCaptureMaxPacket48k = 168;
constexpr UInt32 kPlaybackMaxPacket48k = 360;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kPlaybackSlots = 128;
constexpr std::size_t kHalfPackets = kPlaybackSlots / 2;
constexpr std::size_t kPcmChannels = 10;
constexpr std::size_t kPcmCapacityFrames = 8192;
constexpr UInt32 kCycleLead = 256;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr double kRunSeconds = 8.0;
constexpr UInt32 kMaxAudioFrames = 4096;

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

struct CoreAudioInput {
    AudioUnit unit = nullptr;
    AudioDeviceID device = kAudioObjectUnknown;
    macfw::PcmRingBuffer* ring = nullptr;
    std::vector<Float32> mono;
    std::vector<std::int32_t> mapped;
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> renderedFrames{0};
    std::atomic<std::uint64_t> writtenFrames{0};
    std::atomic<std::uint64_t> droppedFrames{0};
    std::atomic<std::uint64_t> renderErrors{0};

    ~CoreAudioInput() { stop(); }

    static OSStatus inputCallback(void* refCon,
                                  AudioUnitRenderActionFlags* flags,
                                  const AudioTimeStamp* ts,
                                  UInt32,
                                  UInt32 frames,
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
            return err;
        }
        self->renderedFrames += frames;

        std::fill(self->mapped.begin(), self->mapped.begin() + frames * kPcmChannels, 0);
        for (UInt32 i = 0; i < frames; ++i) {
            const double x = std::max(-1.0, std::min(1.0, static_cast<double>(self->mono[i])));
            const auto s24 = static_cast<std::int32_t>(x * 8388607.0);
            // Duplicate default CoreAudio mono input to FW410 physical Analog Outputs 1/2.
            self->mapped[i * kPcmChannels + 1] = s24; // PCM position 2 -> Analog Out 1.
            self->mapped[i * kPcmChannels + 6] = s24; // PCM position 7 -> Analog Out 2.
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
        if (!comp) { std::cerr << "AUHAL component not found\n"; return false; }
        OSStatus err = AudioComponentInstanceNew(comp, &unit);
        if (err != noErr) { std::cerr << "AudioComponentInstanceNew: " << osStatusText(err) << '\n'; return false; }

        UInt32 one = 1, zero = 0;
        err = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                                   kAudioUnitScope_Input, 1, &one, sizeof(one));
        if (err != noErr) { std::cerr << "enable AUHAL input: " << osStatusText(err) << '\n'; return false; }
        err = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                                   kAudioUnitScope_Output, 0, &zero, sizeof(zero));
        if (err != noErr) { std::cerr << "disable AUHAL output: " << osStatusText(err) << '\n'; return false; }

        AudioObjectPropertyAddress addr{
            kAudioHardwarePropertyDefaultInputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMaster
        };
        UInt32 size = sizeof(device);
        err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &device);
        if (err != noErr || device == kAudioObjectUnknown) {
            std::cerr << "default CoreAudio input device unavailable: " << osStatusText(err) << '\n';
            return false;
        }
        err = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                   kAudioUnitScope_Global, 0, &device, sizeof(device));
        if (err != noErr) { std::cerr << "bind AUHAL input device: " << osStatusText(err) << '\n'; return false; }

        AudioStreamBasicDescription asbd{};
        asbd.mSampleRate = 48000.0;
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
        asbd.mBytesPerPacket = sizeof(Float32);
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerFrame = sizeof(Float32);
        asbd.mChannelsPerFrame = 1;
        asbd.mBitsPerChannel = 32;
        err = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Output, 1, &asbd, sizeof(asbd));
        if (err != noErr) {
            std::cerr << "set AUHAL client format 48k mono Float32: " << osStatusText(err) << '\n';
            return false;
        }

        UInt32 maxFrames = kMaxAudioFrames;
        AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
                             kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames));

        AURenderCallbackStruct cb{};
        cb.inputProc = &CoreAudioInput::inputCallback;
        cb.inputProcRefCon = this;
        err = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback,
                                   kAudioUnitScope_Global, 0, &cb, sizeof(cb));
        if (err != noErr) { std::cerr << "set AUHAL input callback: " << osStatusText(err) << '\n'; return false; }

        err = AudioUnitInitialize(unit);
        if (err != noErr) { std::cerr << "AudioUnitInitialize: " << osStatusText(err) << '\n'; return false; }
        return true;
    }

    bool start() {
        const OSStatus err = AudioOutputUnitStart(unit);
        if (err != noErr) { std::cerr << "AudioOutputUnitStart: " << osStatusText(err) << '\n'; return false; }
        return true;
    }

    void stop() {
        if (!unit) return;
        AudioOutputUnitStop(unit);
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        unit = nullptr;
    }
};

void dumpCapture(const macfw::AmdtpReceiveRing& ring) {
    std::size_t touched = 0, dataBearing = 0;
    for (std::size_t i = 0; i < ring.packetCount(); ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched()) continue;
        ++touched;
        if (slot.packet().length > 8) ++dataBearing;
    }
    std::cout << "capture summary:\n"
              << "    touched slots:      " << touched << " / " << ring.packetCount() << '\n'
              << "    data-bearing slots: " << dataBearing << '\n'
              << "result: " << (dataBearing ? "PASS" : "FAIL")
              << (dataBearing ? " - sample-bearing capture observed" : " - no sample-bearing capture") << '\n';
}

bool run(bool execute) {
    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) { std::cout << "No operational FW 410 unit found.\n"; return false; }
    std::cout << "FW410 operational unit:\n"
              << "    generation: " << device.generation() << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID() << std::dec << '\n';
    if (device.open() != kIOReturnSuccess) { std::cout << "open failed\n"; return false; }

    std::uint32_t opcr0 = 0, ipcr0 = 0;
    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) {
        std::cout << "PCR read failed\n"; return false;
    }
    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n"; return false;
    }

    auto native = device.nativeHandle();
    UInt32 cycleTime = 0;
    if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) return false;
    const UInt32 initialCycle = cycleCount(cycleTime);
    const UInt32 firstCycle = (initialCycle + kCycleLead) % kCyclesPerSecond;

    std::cout << "CoreAudio bridge:\n"
              << "    CoreAudio source:   default input device\n"
              << "    client format:      48000 Hz mono Float32\n"
              << "    FW410 destinations: Analog Out 1 + Analog Out 2 (duplicated mono)\n"
              << "    PCM FIFO:           " << kPcmCapacityFrames << " frames\n"
              << "    scheduler:          AmdtpPcmStream48k\n"
              << "    observation window: " << kRunSeconds << " s\n";
    if (!execute) {
        std::cout << "status: PASS - dry run only; no CoreAudio or FireWire stream started\n"
                  << "to execute: ./coreaudiobridge --execute\n";
        return true;
    }

    macfw::PcmRingBuffer pcm(kPcmCapacityFrames, kPcmChannels);
    CoreAudioInput input;
    if (!pcm.valid() || !input.configure(pcm)) return false;

    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket48k);
    auto tx = macfw::AmdtpTransmitRing::createSilence48k(device, firstCycle, kPlaybackSlots);
    auto capture = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::DeviceToHost, kCaptureMaxPacket48k);
    auto playback = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::HostToDevice, kPlaybackMaxPacket48k);
    if (!rx || !tx || !capture || !playback) { std::cout << "transport object creation failed\n"; return false; }

    if (!input.start()) return false;
    const CFAbsoluteTime prefillDeadline = CFAbsoluteTimeGetCurrent() + 1.0;
    while (pcm.availableFrames() < 1024 && CFAbsoluteTimeGetCurrent() < prefillDeadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, false);

    macfw::AmdtpPcmStream48k streamer(tx, pcm, initialCycle, firstCycle, kHalfPackets);
    if (!streamer.valid() || !streamer.prime()) { std::cout << "TX stream scheduler prime failed\n"; return false; }

    (*capture.nativeChannel())->AddListener(
        capture.nativeChannel(), reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
    if (playback.bindHostToDeviceTalkerFirst(tx.nativeLocalPort()) != kIOReturnSuccess) {
        std::cout << "playback port binding failed\n"; return false;
    }

    bool callbackDispatcher = false, isochDispatcher = false, notifications = false;
    bool captureStarted = false, playbackStarted = false, opConnected = false, ipConnected = false;
    bool ok = false;
    if ((*native)->AddCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) callbackDispatcher = true;
    if ((*native)->AddIsochCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) isochDispatcher = true;
    if ((*native)->TurnOnNotification(native)) notifications = true;

    if (capture.allocate() != kIOReturnSuccess || playback.allocate() != kIOReturnSuccess) {
        std::cout << "ISO resource allocation failed\n"; goto cleanup;
    }
    std::cout << "ISO resources:\n"
              << "    capture channel:  " << capture.channel() << '\n'
              << "    playback channel: " << playback.channel() << '\n';
    if (macfw::cmp::connectOpcr0(device, opcr0, capture.channel(), capture.speed()) != kIOReturnSuccess) goto cleanup;
    opConnected = true;
    if (macfw::cmp::connectIpcr0(device, ipcr0, playback.channel()) != kIOReturnSuccess) goto cleanup;
    ipConnected = true;
    if ((*playback.nativeChannel())->Start(playback.nativeChannel()) != kIOReturnSuccess) goto cleanup;
    playbackStarted = true;
    if ((*capture.nativeChannel())->Start(capture.nativeChannel()) != kIOReturnSuccess) goto cleanup;
    captureStarted = true;

    std::cout << "bridge started: speak/play into the Mac's default input; listen on FW410 Out 1/2\n";
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
        const auto& stats = streamer.stats();
        std::cout << "bridge statistics:\n"
                  << "    CoreAudio callbacks: " << input.callbacks.load() << '\n'
                  << "    CoreAudio frames:    " << input.renderedFrames.load() << '\n'
                  << "    PCM written frames:  " << input.writtenFrames.load() << '\n'
                  << "    PCM dropped frames:  " << input.droppedFrames.load() << '\n'
                  << "    CoreAudio errors:    " << input.renderErrors.load() << '\n'
                  << "    PCM consumed frames: " << pcm.consumedFrames() << '\n'
                  << "    PCM underrun frames: " << pcm.underrunFrames() << '\n'
                  << "    TX halves refilled:  " << stats.halvesRefilled << '\n'
                  << "    late cycle polls:    " << stats.lateCyclePolls << '\n';
    }
    ok = true;

cleanup:
    input.stop();
    if (playbackStarted) (*playback.nativeChannel())->Stop(playback.nativeChannel());
    if (captureStarted) (*capture.nativeChannel())->Stop(capture.nativeChannel());
    if (ipConnected) {
        const auto kr = macfw::cmp::restore(device, macfw::cmp::kIpcr0AddressLo, ipcr0);
        std::cout << "restore iPCR[0]: " << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
    }
    if (opConnected) {
        const auto kr = macfw::cmp::restore(device, macfw::cmp::kOpcr0AddressLo, opcr0);
        std::cout << "restore oPCR[0]: " << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
    }
    playback.release(); capture.release();
    if (notifications) (*native)->TurnOffNotification(native);
    if (isochDispatcher) (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (callbackDispatcher) (*native)->RemoveCallbackDispatcherFromRunLoop(native);

    std::uint32_t opAfter = 0, ipAfter = 0;
    if (macfw::cmp::readOpcr0(device, opAfter) == kIOReturnSuccess &&
        macfw::cmp::readIpcr0(device, ipAfter) == kIOReturnSuccess) {
        std::cout << "post-test PCR restore: "
                  << ((opAfter == opcr0 && ipAfter == ipcr0) ? "PASS" : "FAIL") << '\n';
    }
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else { std::cerr << "usage: ./coreaudiobridge [--execute]\n"; return 64; }
    }
    std::cout << "macfw coreaudiobridge — CoreAudio input to FW410 PCM bridge\n\n";
    return run(execute) ? 0 : 1;
}
