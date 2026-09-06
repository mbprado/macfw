#include "../hal/include/macfw_fw1814_capture_shm.h"
#include "../hal/include/macfw_fw1814_hal_shm.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct PlaybackMapping {
    int fd = -1;
    macfw::fw1814::hal::SharedPlaybackRing* ring = nullptr;
    ~PlaybackMapping() {
        if (ring) munmap(ring, sizeof(*ring));
        if (fd >= 0) close(fd);
    }
};

struct CaptureMapping {
    int fd = -1;
    macfw::fw1814::hal::capture::SharedCaptureRing* ring = nullptr;
    ~CaptureMapping() {
        if (ring) munmap(ring, sizeof(*ring));
        if (fd >= 0) close(fd);
    }
};

bool initPlayback() {
    const char* name = macfw::fw1814::hal::kPlaybackShmName;
    const int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        std::cerr << "shm_open(" << name << ") failed: "
                  << std::strerror(errno) << " (errno=" << errno << ")\n";
        return false;
    }

    // Match the already-working FW410 HAL creation path: shm_open ->
    // ftruncate -> mmap. fchmod is unnecessary for this temporary same-user
    // producer/consumer test and was the only extra syscall in the FW1814
    // initializer.
    const std::size_t bytes = sizeof(macfw::fw1814::hal::SharedPlaybackRing);
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        std::cerr << "ftruncate(" << name << ", " << bytes << ") failed: "
                  << std::strerror(errno) << " (errno=" << errno << ")\n";
        close(fd);
        return false;
    }

    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        std::cerr << "mmap(" << name << ", " << bytes << ") failed: "
                  << std::strerror(errno) << " (errno=" << errno << ")\n";
        close(fd);
        return false;
    }

    auto* ring = static_cast<macfw::fw1814::hal::SharedPlaybackRing*>(p);
    macfw::fw1814::hal::initialize(*ring, 48000);
    munmap(p, bytes);
    close(fd);
    std::cout << "initialized " << name
              << " as 48000 Hz / 4 physical analog outputs (" << bytes
              << " bytes)\n";
    return true;
}

bool openPlayback(PlaybackMapping& m) {
    m.fd = shm_open(macfw::fw1814::hal::kPlaybackShmName, O_RDWR, 0);
    if (m.fd < 0) return false;
    void* p = mmap(nullptr, sizeof(macfw::fw1814::hal::SharedPlaybackRing),
                   PROT_READ | PROT_WRITE, MAP_SHARED, m.fd, 0);
    if (p == MAP_FAILED) return false;
    m.ring = static_cast<macfw::fw1814::hal::SharedPlaybackRing*>(p);
    return macfw::fw1814::hal::valid(*m.ring);
}

bool openCapture(CaptureMapping& m) {
    m.fd = shm_open(macfw::fw1814::hal::capture::kShmName, O_RDWR, 0);
    if (m.fd < 0) return false;
    void* p = mmap(nullptr, sizeof(macfw::fw1814::hal::capture::SharedCaptureRing),
                   PROT_READ | PROT_WRITE, MAP_SHARED, m.fd, 0);
    if (p == MAP_FAILED) return false;
    m.ring = static_cast<macfw::fw1814::hal::capture::SharedCaptureRing*>(p);
    return macfw::fw1814::hal::capture::valid(*m.ring);
}

bool tone(unsigned output) {
    if (output < 1 || output > macfw::fw1814::hal::kOutputChannels)
        return false;
    PlaybackMapping m;
    if (!openPlayback(m)) {
        std::cerr << "playback SHM unavailable; run --init first\n";
        return false;
    }
    if (m.ring->sampleRate.load(std::memory_order_acquire) != 48000) {
        std::cerr << "playback SHM is not 48000 Hz\n";
        return false;
    }
    if (m.ring->active.load(std::memory_order_acquire) == 0) {
        std::cerr << "FW1814 analog engine is not consuming the playback ring\n";
        return false;
    }

    constexpr std::size_t kChunkFrames = 240; // 5 ms at 48 kHz
    constexpr double kFrequency = 500.0;
    constexpr double kAmplitude = 0.06309573444801933; // -24 dBFS peak
    constexpr unsigned kChunks = 600; // 3 seconds
    std::vector<float> block(
        kChunkFrames * macfw::fw1814::hal::kOutputChannels, 0.0f);
    std::uint64_t sampleIndex = 0;

    std::cout << "500 Hz / -24 dBFS -> physical Analog Output " << output
              << " for 3 seconds\n";
    for (unsigned chunk = 0; chunk < kChunks; ++chunk) {
        for (std::size_t frame = 0; frame < kChunkFrames; ++frame) {
            std::fill_n(block.data() + frame * macfw::fw1814::hal::kOutputChannels,
                        macfw::fw1814::hal::kOutputChannels, 0.0f);
            const double phase = 2.0 * kPi * kFrequency *
                static_cast<double>(sampleIndex++) / 48000.0;
            block[frame * macfw::fw1814::hal::kOutputChannels + (output - 1)] =
                static_cast<float>(std::sin(phase) * kAmplitude);
        }

        std::size_t offset = 0;
        while (offset < kChunkFrames) {
            const std::size_t wrote = macfw::fw1814::hal::write(
                *m.ring,
                block.data() + offset * macfw::fw1814::hal::kOutputChannels,
                kChunkFrames - offset);
            offset += wrote;
            if (offset < kChunkFrames)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

bool captureMeter(unsigned seconds) {
    CaptureMapping m;
    if (!openCapture(m)) {
        std::cerr << "capture SHM unavailable; start fw1814analog48 first\n";
        return false;
    }

    constexpr std::size_t kFrames = 256;
    std::vector<float> block(
        kFrames * macfw::fw1814::hal::capture::kInputChannels, 0.0f);
    std::array<float, macfw::fw1814::hal::capture::kInputChannels> peaks{};
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    auto nextPrint = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

    while (std::chrono::steady_clock::now() < end) {
        // Match the intended HAL contract: merely announcing read callbacks is
        // enough for the producer to notice a consumer. Do not drain capture
        // before active=1, otherwise the producer can never accumulate its
        // required prefill.
        m.ring->halReadCalls.fetch_add(1, std::memory_order_relaxed);
        m.ring->halRequestedFrames.fetch_add(kFrames, std::memory_order_relaxed);

        std::size_t got = 0;
        const bool active =
            m.ring->active.load(std::memory_order_acquire) != 0;
        if (active) {
            got = macfw::fw1814::hal::capture::read(
                *m.ring, block.data(), kFrames);
            m.ring->halFramesFromRing.fetch_add(got, std::memory_order_relaxed);
        }
        if (got < kFrames)
            m.ring->halZeroFilledFrames.fetch_add(
                kFrames - got, std::memory_order_relaxed);

        for (std::size_t frame = 0; frame < got; ++frame) {
            for (std::size_t ch = 0;
                 ch < macfw::fw1814::hal::capture::kInputChannels; ++ch) {
                peaks[ch] = std::max(peaks[ch], std::fabs(
                    block[frame * macfw::fw1814::hal::capture::kInputChannels + ch]));
            }
        }

        if (std::chrono::steady_clock::now() >= nextPrint) {
            std::cout << "capture peaks:";
            for (std::size_t ch = 0; ch < peaks.size(); ++ch)
                std::cout << " A" << (ch + 1) << '=' << peaks[ch];
            std::cout << " queued="
                      << macfw::fw1814::hal::capture::availableFrames(*m.ring)
                      << " active=" << (active ? 1 : 0)
                      << '\n';
            peaks.fill(0.0f);
            nextPrint += std::chrono::milliseconds(500);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void usage(const char* argv0) {
    std::cerr << "usage:\n"
              << "  " << argv0 << " --init\n"
              << "  " << argv0 << " --tone <1..4>\n"
              << "  " << argv0 << " --capture-meter [seconds]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 64;
    }
    const std::string arg = argv[1];
    if (arg == "--init" && argc == 2)
        return initPlayback() ? 0 : 1;
    if (arg == "--tone" && argc == 3) {
        try {
            return tone(static_cast<unsigned>(std::stoul(argv[2]))) ? 0 : 1;
        } catch (...) {
            usage(argv[0]);
            return 64;
        }
    }
    if (arg == "--capture-meter" && (argc == 2 || argc == 3)) {
        unsigned seconds = 10;
        if (argc == 3) {
            try { seconds = static_cast<unsigned>(std::stoul(argv[2])); }
            catch (...) { usage(argv[0]); return 64; }
        }
        return captureMeter(seconds) ? 0 : 1;
    }
    usage(argv[0]);
    return 64;
}
