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

enum class FileChannel { Left, Right, Mix };

// Convert a local mono/stereo audio file to 96 kHz, mix it to one output,
// and pad the preloaded PCM ring with silence. ExtAudioFile performs format
// and sample-rate conversion; no output AudioDevice is opened.
inline bool preloadFile96(const std::string& path,
                          std::vector<std::int32_t>& pcm,
                          std::size_t pcmChannels,
                          std::size_t position,
                          FileChannel channel,
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
    std::vector<float> selected(kMaximumAudioFrames, 0.0f);
    double channelPeak[2] = {};
    double selectedPeak = 0.0;
    double selectedSumSquares = 0.0;
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
            for (UInt32 ch = 0; ch < source.mChannelsPerFrame; ++ch) {
                const float value = block[frame * source.mChannelsPerFrame + ch];
                if (!std::isfinite(value)) {
                    ExtAudioFileDispose(file);
                    return false;
                }
                channelPeak[ch] = std::max(channelPeak[ch],
                                           std::abs(static_cast<double>(value)));
            }
            const float left = block[frame * source.mChannelsPerFrame];
            const float right = source.mChannelsPerFrame == 2
                ? block[frame * source.mChannelsPerFrame + 1] : left;
            const double value = channel == FileChannel::Left ? left
                : channel == FileChannel::Right ? right
                : (static_cast<double>(left) + right) * 0.5;
            selected[audioFrames + frame] = static_cast<float>(value);
            selectedPeak = std::max(selectedPeak, std::abs(value));
            selectedSumSquares += value * value;
        }
        audioFrames += received;
    }
    ExtAudioFileDispose(file);
    if (audioFrames == 0 || selectedPeak < 1e-7) {
        std::cerr << "selected file channel is silent or nearly silent\n";
        return false;
    }
    // Match the hardware-proven tone's -24 dBFS peak. This also avoids a
    // quiet source disappearing under an extra fixed gain reduction.
    const double scale = 529285.0 / selectedPeak;
    std::size_t nonzeroFrames = 0;
    for (std::size_t frame = 0; frame < audioFrames; ++frame) {
        const auto sample = static_cast<std::int32_t>(selected[frame] * scale);
        pcm[frame * pcmChannels + position] = sample;
        nonzeroFrames += sample != 0;
    }
    std::cout << "file source: " << source.mSampleRate << " Hz, "
              << source.mChannelsPerFrame << " channel(s)\n"
              << "source channel peak L/R: " << channelPeak[0] << " / "
              << channelPeak[1] << '\n'
              << "selected channel: "
              << (channel == FileChannel::Left ? "left" :
                  channel == FileChannel::Right ? "right" : "mix") << '\n'
              << "selected peak/RMS: " << selectedPeak << " / "
              << std::sqrt(selectedSumSquares / audioFrames) << '\n'
              << "converted audio: " << audioFrames << " frames at 96000 Hz"
              << " (" << nonzeroFrames << " nonzero, normalized to -24 dBFS peak)\n";
    return nonzeroFrames > 0;
}

} // namespace macfw::fw1814::experimental
