#include "macfw_hal_capture_shm.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

int main() {
    const int fd = shm_open(macfw::hal::capture::kShmName, O_RDWR, 0);
    if (fd < 0) {
        std::fprintf(stderr, "capture shared ring unavailable; run capturebridge48000 first\n");
        return 1;
    }
    void* p = mmap(nullptr, sizeof(macfw::hal::capture::SharedCaptureRing),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return 1;
    }

    auto* ring = static_cast<macfw::hal::capture::SharedCaptureRing*>(p);
    if (!macfw::hal::capture::valid(*ring)) {
        std::fprintf(stderr, "capture shared ring invalid\n");
        munmap(p, sizeof(*ring));
        close(fd);
        return 2;
    }

    constexpr std::size_t kChunkFrames = 4096;
    std::vector<float> buffer(kChunkFrames * macfw::hal::capture::kChannels, 0.0f);
    std::array<double, macfw::hal::capture::kChannels> sumSquares{};
    std::array<float, macfw::hal::capture::kChannels> peak{};
    std::uint64_t observedFrames = 0;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::size_t available = macfw::hal::capture::availableFrames(*ring);
        const std::size_t wanted = std::min<std::size_t>(available, kChunkFrames);
        if (!wanted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        const std::size_t got = macfw::hal::capture::read(*ring, buffer.data(), wanted);
        observedFrames += got;
        for (std::size_t frame = 0; frame < got; ++frame) {
            for (std::size_t ch = 0; ch < macfw::hal::capture::kChannels; ++ch) {
                const float v = buffer[frame * macfw::hal::capture::kChannels + ch];
                peak[ch] = std::max(peak[ch], std::fabs(v));
                sumSquares[ch] += static_cast<double>(v) * static_cast<double>(v);
            }
        }
    }

    static constexpr const char* names[macfw::hal::capture::kChannels] = {
        "Analog In 1", "Analog In 2", "S/PDIF In L", "S/PDIF In R"
    };

    std::printf("macfw captureprobe — 2 s FW410 capture level window\n");
    std::printf("    sample rate:      %u Hz\n", ring->sampleRate.load(std::memory_order_acquire));
    std::printf("    active:           %u\n", ring->active.load(std::memory_order_acquire));
    std::printf("    observed frames:  %llu\n", static_cast<unsigned long long>(observedFrames));
    std::printf("    decoded packets:  %llu\n", static_cast<unsigned long long>(ring->decodedPackets.load(std::memory_order_acquire)));
    std::printf("    decoded frames:   %llu\n", static_cast<unsigned long long>(ring->decodedFrames.load(std::memory_order_acquire)));
    std::printf("    dropped frames:   %llu\n", static_cast<unsigned long long>(ring->droppedFrames.load(std::memory_order_acquire)));
    std::printf("    malformed:        %llu\n", static_cast<unsigned long long>(ring->malformedPackets.load(std::memory_order_acquire)));
    std::printf("    invalid labels:   %llu\n", static_cast<unsigned long long>(ring->invalidLabels.load(std::memory_order_acquire)));

    for (std::size_t ch = 0; ch < macfw::hal::capture::kChannels; ++ch) {
        const double rms = observedFrames ? std::sqrt(sumSquares[ch] / static_cast<double>(observedFrames)) : 0.0;
        std::printf("    ch %zu %-12s peak=%0.6f rms=%0.6f\n", ch + 1, names[ch], peak[ch], rms);
    }

    munmap(p, sizeof(*ring));
    close(fd);
    return 0;
}
