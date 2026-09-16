#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace macfw::fw1814::experimental {

// Convert a local mono/stereo audio file to 96 kHz, mix it to one output,
// and pad the preloaded PCM ring with silence. ExtAudioFile performs format
// and sample-rate conversion; no output AudioDevice is opened.
inline bool preloadFile96(const std::string& path,
                          std::vector<std::int32_t>& pcm,
                          std::size_t pcmChannels,
                          std::size_t position,
                          std::size_t& audioFrames) {
    audioFrames = 0;
    if (path.empty() || path.front() != '/' || pcmChannels == 0 ||
        position >= pcmChannels || pcm.size() % pcmChannels != 0)
        return false;

    auto url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.data()),
        static_cast<CFIndex>(path.size()), false);
    if (!url) return false;
    ExtAudioFileRef file = nullptr;
    const OSStatus openStatus = ExtAudioFileOpenURL(url, &file);
    CFRelease(url);
    if (openStatus != noErr || !file) {
        std::cerr << "AudioToolbox file open failed: " << openStatus << '\n';
        return false;
    }

    AudioStreamBasicDescription source{};
    UInt32 propertySize = sizeof(source);
    OSStatus status = ExtAudioFileGetProperty(file,
        kExtAudioFileProperty_FileDataFormat, &propertySize, &source);
    if (status != noErr || source.mChannelsPerFrame < 1 ||
        source.mChannelsPerFrame > 2) {
        std::cerr << "file format unavailable or not mono/stereo: " << status << '\n';
        ExtAudioFileDispose(file);
        return false;
    }

    AudioStreamBasicDescription client{};
    client.mSampleRate = 96000.0;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    client.mChannelsPerFrame = source.mChannelsPerFrame;
    client.mBitsPerChannel = 32;
    client.mFramesPerPacket = 1;
    client.mBytesPerFrame = source.mChannelsPerFrame * sizeof(float);
    client.mBytesPerPacket = client.mBytesPerFrame;
    status = ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat,
                                    sizeof(client), &client);
    if (status != noErr) {
        std::cerr << "96 kHz file conversion setup failed: " << status << '\n';
        ExtAudioFileDispose(file);
        return false;
    }

    constexpr std::size_t kBlockFrames = 4096;
    constexpr std::size_t kMaximumAudioFrames = 3 * 96000;
    std::vector<float> block(kBlockFrames * source.mChannelsPerFrame);
    const std::size_t limit = std::min(kMaximumAudioFrames,
                                        pcm.size() / pcmChannels);
    while (audioFrames < limit) {
        const auto request = static_cast<UInt32>(
            std::min(kBlockFrames, limit - audioFrames));
        AudioBufferList buffers{};
        buffers.mNumberBuffers = 1;
        buffers.mBuffers[0].mNumberChannels = source.mChannelsPerFrame;
        buffers.mBuffers[0].mDataByteSize = request * client.mBytesPerFrame;
        buffers.mBuffers[0].mData = block.data();
        UInt32 received = request;
        status = ExtAudioFileRead(file, &received, &buffers);
        if (status != noErr) {
            std::cerr << "96 kHz file conversion failed: " << status << '\n';
            ExtAudioFileDispose(file);
            return false;
        }
        if (received == 0) break;
        for (UInt32 frame = 0; frame < received; ++frame) {
            double mixed = 0.0;
            for (UInt32 ch = 0; ch < source.mChannelsPerFrame; ++ch) {
                const float value = block[frame * source.mChannelsPerFrame + ch];
                if (!std::isfinite(value)) {
                    ExtAudioFileDispose(file);
                    return false;
                }
                mixed += value;
            }
            mixed /= source.mChannelsPerFrame;
            // -12 dB from source peak, capped to the 24-bit AM824 range.
            const double scaled = std::clamp(mixed * 0.25, -1.0, 1.0);
            pcm[(audioFrames + frame) * pcmChannels + position] =
                static_cast<std::int32_t>(scaled * 8388607.0);
        }
        audioFrames += received;
    }
    ExtAudioFileDispose(file);
    std::cout << "file source: " << source.mSampleRate << " Hz, "
              << source.mChannelsPerFrame << " channel(s)\n"
              << "converted audio: " << audioFrames << " frames at 96000 Hz"
              << " (mono mix, -12 dB; remaining buffer silent)\n";
    return audioFrames > 0;
}

} // namespace macfw::fw1814::experimental
