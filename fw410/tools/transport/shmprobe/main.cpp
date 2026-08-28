#include "../../../hal/include/macfw_hal_shm.h"
#include "../../../hal/include/macfw_hal_capture_shm.h"

#include <CoreAudio/AudioServerPlugIn.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

const char* operationName(std::uint32_t op) {
    switch (op) {
        case kAudioServerPlugInIOOperationReadInput: return "ReadInput";
        case kAudioServerPlugInIOOperationProcessInput: return "ProcessInput";
        case kAudioServerPlugInIOOperationProcessOutput: return "ProcessOutput";
        case kAudioServerPlugInIOOperationWriteMix: return "WriteMix";
        case 0: return "none";
        default: return "unknown";
    }
}

void printOperation(const char* label, std::uint32_t op) {
    std::printf("    %-18s %u (%s)\n", label, op, operationName(op));
}

void printPlaybackPeaks(const macfw::hal::SharedPcmRing& ring) {
    static constexpr std::array<const char*, macfw::hal::kChannels> kNames = {
        "Analog 1", "Analog 2", "Analog 3", "Analog 4", "Analog 5",
        "Analog 6", "Analog 7", "Analog 8", "S/PDIF L", "S/PDIF R"
    };
    constexpr std::size_t kSnapshotFrames = 512;

    const auto w = ring.writeFrame.load(std::memory_order_acquire);
    const auto r = ring.readFrame.load(std::memory_order_acquire);
    const std::uint64_t available = w - r;
    const std::size_t frames = static_cast<std::size_t>(
        std::min<std::uint64_t>(available, kSnapshotFrames));

    std::array<float, macfw::hal::kChannels> peaks{};
    const std::uint64_t start = w - frames;
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t base = static_cast<std::size_t>(
            (start + i) % macfw::hal::kCapacityFrames) * macfw::hal::kChannels;
        for (std::size_t ch = 0; ch < macfw::hal::kChannels; ++ch)
            peaks[ch] = std::max(peaks[ch], std::fabs(ring.samples[base + ch]));
    }

    std::printf("Playback channel peak snapshot (%zu buffered frames):\n", frames);
    for (std::size_t ch = 0; ch < peaks.size(); ++ch)
        std::printf("    ch %2zu %-10s %.6f\n", ch + 1, kNames[ch], peaks[ch]);
}

void printCaptureState() {
    const int fd = shm_open(macfw::hal::capture::kShmName, O_RDWR, 0);
    if (fd < 0) {
        std::printf("Capture shared ring: unavailable (%s)\n", std::strerror(errno));
        return;
    }
    void* p = mmap(nullptr, sizeof(macfw::hal::capture::SharedCaptureRing),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        std::printf("Capture shared ring: mmap failed (%s)\n", std::strerror(errno));
        close(fd);
        return;
    }

    auto* ring = static_cast<macfw::hal::capture::SharedCaptureRing*>(p);
    const bool valid = macfw::hal::capture::valid(*ring);
    std::printf("Capture shared ring: %s\n", valid ? "PASS" : "INVALID");
    if (valid) {
        std::printf("Capture state:\n");
        std::printf("    sample rate:        %u Hz\n", ring->sampleRate.load(std::memory_order_acquire));
        std::printf("    active:             %u\n", ring->active.load(std::memory_order_acquire));
        std::printf("    write frame:        %llu\n", static_cast<unsigned long long>(ring->writeFrame.load(std::memory_order_acquire)));
        std::printf("    read frame:         %llu\n", static_cast<unsigned long long>(ring->readFrame.load(std::memory_order_acquire)));
        std::printf("    available:          %llu frames\n", static_cast<unsigned long long>(macfw::hal::capture::availableFrames(*ring)));
        std::printf("    dropped frames:     %llu\n", static_cast<unsigned long long>(ring->droppedFrames.load(std::memory_order_acquire)));
        std::printf("    decoded packets:    %llu\n", static_cast<unsigned long long>(ring->decodedPackets.load(std::memory_order_acquire)));
        std::printf("    decoded frames:     %llu\n", static_cast<unsigned long long>(ring->decodedFrames.load(std::memory_order_acquire)));
        std::printf("    malformed:          %llu\n", static_cast<unsigned long long>(ring->malformedPackets.load(std::memory_order_acquire)));
        std::printf("    invalid labels:     %llu\n", static_cast<unsigned long long>(ring->invalidLabels.load(std::memory_order_acquire)));
        std::printf("HAL capture consumption:\n");
        std::printf("    ReadInput calls:    %llu\n", static_cast<unsigned long long>(ring->halReadCalls.load(std::memory_order_acquire)));
        std::printf("    requested frames:   %llu\n", static_cast<unsigned long long>(ring->halRequestedFrames.load(std::memory_order_acquire)));
        std::printf("    frames from ring:   %llu\n", static_cast<unsigned long long>(ring->halFramesFromRing.load(std::memory_order_acquire)));
        std::printf("    zero-filled frames: %llu\n", static_cast<unsigned long long>(ring->halZeroFilledFrames.load(std::memory_order_acquire)));
    }

    munmap(p, sizeof(*ring));
    close(fd);
}

} // namespace

int main() {
    const int fd = shm_open(macfw::hal::kShmName, O_RDWR, 0);
    if (fd < 0) {
        std::fprintf(stderr, "HAL shared ring unavailable: shm_open(%s) failed: %s\n",
                     macfw::hal::kShmName, std::strerror(errno));
        return 1;
    }

    void* p = mmap(nullptr, sizeof(macfw::hal::SharedPcmRing), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        std::fprintf(stderr, "HAL shared ring mmap failed: %s\n", std::strerror(errno));
        close(fd);
        return 1;
    }

    auto* ring = static_cast<macfw::hal::SharedPcmRing*>(p);
    const bool isValid = macfw::hal::valid(*ring);

    std::printf("HAL shared ring: %s\n", isValid ? "PASS" : "INVALID");
    if (isValid) {
        std::printf("PCM state:\n");
        std::printf("    sample rate:        %u Hz\n", ring->sampleRate.load(std::memory_order_acquire));
        std::printf("    active:             %u\n", ring->active.load(std::memory_order_acquire));
        std::printf("    write frame:        %llu\n", static_cast<unsigned long long>(ring->writeFrame.load(std::memory_order_acquire)));
        std::printf("    read frame:         %llu\n", static_cast<unsigned long long>(ring->readFrame.load(std::memory_order_acquire)));
        std::printf("    available:          %llu frames\n", static_cast<unsigned long long>(macfw::hal::availableFrames(*ring)));
        std::printf("    dropped frames:     %llu\n", static_cast<unsigned long long>(ring->droppedFrames.load(std::memory_order_acquire)));
        std::printf("    underrun frames:    %llu\n", static_cast<unsigned long long>(ring->underrunFrames.load(std::memory_order_acquire)));

        printPlaybackPeaks(*ring);

        std::printf("HAL I/O instrumentation:\n");
        std::printf("    StartIO calls:      %llu\n", static_cast<unsigned long long>(ring->startIOCalls.load(std::memory_order_acquire)));
        std::printf("    StopIO calls:       %llu\n", static_cast<unsigned long long>(ring->stopIOCalls.load(std::memory_order_acquire)));
        std::printf("    WillDo calls:       %llu\n", static_cast<unsigned long long>(ring->willDoCalls.load(std::memory_order_acquire)));
        std::printf("    BeginIO calls:      %llu\n", static_cast<unsigned long long>(ring->beginIOCalls.load(std::memory_order_acquire)));
        std::printf("    DoIO calls:         %llu\n", static_cast<unsigned long long>(ring->doIOCalls.load(std::memory_order_acquire)));
        std::printf("    EndIO calls:        %llu\n", static_cast<unsigned long long>(ring->endIOCalls.load(std::memory_order_acquire)));
        std::printf("    WriteMix DoIO:      %llu\n", static_cast<unsigned long long>(ring->writeMixCalls.load(std::memory_order_acquire)));
        std::printf("    non-null buffers:   %llu\n", static_cast<unsigned long long>(ring->nonNullMainBufferCalls.load(std::memory_order_acquire)));
        std::printf("    DoIO total frames:  %llu\n", static_cast<unsigned long long>(ring->doIOFrames.load(std::memory_order_acquire)));
        std::printf("    last DoIO frames:   %u\n", ring->lastDoFrames.load(std::memory_order_acquire));
        printOperation("last WillDo op:", ring->lastWillDoOperation.load(std::memory_order_acquire));
        printOperation("last BeginIO op:", ring->lastBeginOperation.load(std::memory_order_acquire));
        printOperation("last DoIO op:", ring->lastDoOperation.load(std::memory_order_acquire));
        printOperation("last EndIO op:", ring->lastEndOperation.load(std::memory_order_acquire));
    }

    munmap(p, sizeof(*ring));
    close(fd);

    std::printf("\n");
    printCaptureState();
    return isValid ? 0 : 2;
}
