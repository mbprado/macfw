#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::string statusText(OSStatus s) {
    char t[5] = {};
    const UInt32 u = static_cast<UInt32>(s);
    t[0] = static_cast<char>((u >> 24) & 0xff);
    t[1] = static_cast<char>((u >> 16) & 0xff);
    t[2] = static_cast<char>((u >> 8) & 0xff);
    t[3] = static_cast<char>(u & 0xff);
    bool printable = true;
    for (int i = 0; i < 4; ++i) printable &= t[i] >= 32 && t[i] < 127;
    if (printable) return std::string("'") + std::string(t, 4) + "' (" + std::to_string(s) + ")";
    return std::to_string(s);
}

struct Diag {
    AudioUnit unit = nullptr;
    std::vector<Float32> buffer;
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<OSStatus> firstError{noErr};

    static OSStatus callback(void* ref,
                             AudioUnitRenderActionFlags* flags,
                             const AudioTimeStamp* ts,
                             UInt32 bus,
                             UInt32 nframes,
                             AudioBufferList*) {
        auto* self = static_cast<Diag*>(ref);
        ++self->callbacks;
        if (nframes > self->buffer.size()) return noErr;

        AudioBufferList abl{};
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 1;
        abl.mBuffers[0].mDataByteSize = nframes * sizeof(Float32);
        abl.mBuffers[0].mData = self->buffer.data();

        const OSStatus err = AudioUnitRender(self->unit, flags, ts, bus, nframes, &abl);
        if (err != noErr) {
            ++self->errors;
            OSStatus expected = noErr;
            self->firstError.compare_exchange_strong(expected, err);
            return noErr;
        }
        self->frames += nframes;
        return noErr;
    }
};
}

int main() {
    AudioComponentDescription d{};
    d.componentType = kAudioUnitType_Output;
    d.componentSubType = kAudioUnitSubType_HALOutput;
    d.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent c = AudioComponentFindNext(nullptr, &d);
    if (!c) { std::cerr << "AUHAL component not found\n"; return 1; }

    Diag diag;
    OSStatus err = AudioComponentInstanceNew(c, &diag.unit);
    if (err != noErr) { std::cerr << "AudioComponentInstanceNew: " << statusText(err) << '\n'; return 1; }

    UInt32 one = 1, zero = 0;
    err = AudioUnitSetProperty(diag.unit, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Input, 1, &one, sizeof(one));
    if (err != noErr) { std::cerr << "enable input: " << statusText(err) << '\n'; return 1; }
    err = AudioUnitSetProperty(diag.unit, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Output, 0, &zero, sizeof(zero));
    if (err != noErr) { std::cerr << "disable output: " << statusText(err) << '\n'; return 1; }

    AudioDeviceID device = kAudioObjectUnknown;
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultInputDevice,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMaster};
    UInt32 size = sizeof(device);
    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &device);
    if (err != noErr) { std::cerr << "default input: " << statusText(err) << '\n'; return 1; }
    std::cout << "default input device id: " << device << '\n';

    err = AudioUnitSetProperty(diag.unit, kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0, &device, sizeof(device));
    if (err != noErr) { std::cerr << "set current device: " << statusText(err) << '\n'; return 1; }

    AudioStreamBasicDescription hw{};
    size = sizeof(hw);
    err = AudioUnitGetProperty(diag.unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 1, &hw, &size);
    if (err != noErr) { std::cerr << "get device format: " << statusText(err) << '\n'; return 1; }
    std::cout << "device format: rate=" << hw.mSampleRate
              << " channels=" << hw.mChannelsPerFrame
              << " bits=" << hw.mBitsPerChannel
              << " bytes/frame=" << hw.mBytesPerFrame << '\n';

    AudioStreamBasicDescription client{};
    client.mSampleRate = hw.mSampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    client.mBytesPerPacket = sizeof(Float32);
    client.mFramesPerPacket = 1;
    client.mBytesPerFrame = sizeof(Float32);
    client.mChannelsPerFrame = 1;
    client.mBitsPerChannel = 32;

    err = AudioUnitSetProperty(diag.unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, 1, &client, sizeof(client));
    if (err != noErr) { std::cerr << "set client format: " << statusText(err) << '\n'; return 1; }

    size = sizeof(client);
    err = AudioUnitGetProperty(diag.unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, 1, &client, &size);
    if (err != noErr) { std::cerr << "read client format: " << statusText(err) << '\n'; return 1; }
    std::cout << "client format: rate=" << client.mSampleRate
              << " channels=" << client.mChannelsPerFrame
              << " bits=" << client.mBitsPerChannel
              << " bytes/frame=" << client.mBytesPerFrame << '\n';

    UInt32 maxFrames = 4096;
    AudioUnitSetProperty(diag.unit, kAudioUnitProperty_MaximumFramesPerSlice,
                         kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames));
    diag.buffer.assign(maxFrames, 0.0f);

    AURenderCallbackStruct cb{};
    cb.inputProc = &Diag::callback;
    cb.inputProcRefCon = &diag;
    err = AudioUnitSetProperty(diag.unit, kAudioOutputUnitProperty_SetInputCallback,
                               kAudioUnitScope_Global, 0, &cb, sizeof(cb));
    if (err != noErr) { std::cerr << "set input callback: " << statusText(err) << '\n'; return 1; }

    err = AudioUnitInitialize(diag.unit);
    if (err != noErr) { std::cerr << "initialize: " << statusText(err) << '\n'; return 1; }
    err = AudioOutputUnitStart(diag.unit);
    if (err != noErr) { std::cerr << "start: " << statusText(err) << '\n'; return 1; }

    std::cout << "running AUHAL input diagnostic for 3 seconds...\n";
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 3.0;
    while (CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);

    AudioOutputUnitStop(diag.unit);
    AudioUnitUninitialize(diag.unit);
    AudioComponentInstanceDispose(diag.unit);

    std::cout << "callbacks: " << diag.callbacks.load() << '\n'
              << "rendered frames: " << diag.frames.load() << '\n'
              << "render errors: " << diag.errors.load() << '\n'
              << "first render error: "
              << (diag.firstError.load() == noErr ? "none" : statusText(diag.firstError.load())) << '\n';
    return 0;
}
