#include "../../../hal/include/macfw_hal_shm.h"

#include <CoreAudio/AudioServerPlugIn.h>

#include <cerrno>
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
    return isValid ? 0 : 2;
}
