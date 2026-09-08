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

bool supportedRate(std::uint32_t rate) {
    return rate == 44100 || rate == 48000;
}

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

bool initPlayback(std::uint32_t rate) {
    if (!supportedRate(rate)) return false;
    const char* name = macfw::fw1814::hal::kPlaybackShmName;
    const int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        std::cerr << "shm_open(" << name << ") failed: "
                  << std::strerror(errno) << " (errno=" << errno << ")\n";
        return false;
    }

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
    macfw::fw1814::hal::initialize(*ring, rate);
    munmap(p, bytes);
    close(fd);
    std::cout << "initialized " << name
              << " as " << rate << " Hz / 4 physical analog outputs ("
              << bytes << " bytes)\n";
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

bool tone(unsigned output, double frequency = 500.0) {
    if (output < 1 || output > macfw::fw1814::hal::kOutputChannels ||
        !std::isfinite(frequency) || frequency <= 0.0)
        return false;
    PlaybackMapping m;
    if (!openPlayback(m)) {
        std::cerr << "playback SHM unavailable; run --init [44100|48000] first\n";
        return false;
    }
    const std::uint32_t rate =
        m.ring->sampleRate.load(std::memory_order_acquire);
    if (!supportedRate(rate)) {
        std::cerr << "playback SHM has unsupported sample rate " << rate << " Hz\n";
        return false;
    }
    if (frequency >= static_cast<double>(rate) / 2.0) {
        std::cerr << "tone frequency must be below Nyquist for " << rate << " Hz\n";
        return false;
    }
    if (m.ring->active.load(std::memory_order_acquire) == 0) {
        std::cerr << "FW1814 analog engine is not consuming the playback ring\n";
        return false;
    }

    const std::size_t kChunkFrames = static_cast<std::size_t>(rate / 50); // 20 ms
    const std::size_t kTotalFrames = static_cast<std::size_t>(rate) * 3;
    constexpr double kAmplitude = 0.06309573444801933; // -24 dBFS peak
    std::vector<float> block(
        kChunkFrames * macfw::fw1814::hal::kOutputChannels, 0.0f);
    std::uint64_t sampleIndex = 0;
    std::size_t produced = 0;

    std::cout << frequency << " Hz / -24 dBFS / " << rate
              << " Hz -> physical Analog Output " << output
              << " for 3 seconds (buffered producer)\n";

    while (produced < kTotalFrames) {
        const std::size_t framesThisChunk =
            std::min(kChunkFrames, kTotalFrames - produced);
        for (std::size_t frame = 0; frame < framesThisChunk; ++frame) {
            std::fill_n(block.data() + frame * macfw::fw1814::hal::kOutputChannels,
                        macfw::fw1814::hal::kOutputChannels, 0.0f);
            const double phase = 2.0 * kPi * frequency *
                static_cast<double>(sampleIndex++) / static_cast<double>(rate);
            block[frame * macfw::fw1814::hal::kOutputChannels + (output - 1)] =
                static_cast<float>(std::sin(phase) * kAmplitude);
        }

        std::size_t offset = 0;
        while (offset < framesThisChunk) {
            const std::size_t used = macfw::fw1814::hal::availableFrames(*m.ring);
            const std::size_t free = used >= macfw::fw1814::hal::kCapacityFrames
                ? 0
                : macfw::fw1814::hal::kCapacityFrames - used;
            if (free == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const std::size_t request =
                std::min(free, framesThisChunk - offset);
            const std::size_t wrote = macfw::fw1814::hal::write(
                *m.ring,
                block.data() + offset * macfw::fw1814::hal::kOutputChannels,
                request);
            offset += wrote;
            if (wrote == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        produced += framesThisChunk;
    }

    const std::uint64_t targetRead =
        m.ring->writeFrame.load(std::memory_order_acquire);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (m.ring->readFrame.load(std::memory_order_acquire) < targetRead &&
           std::chrono::steady_clock::now() < deadline) {
        if (m.ring->active.load(std::memory_order_acquire) == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

bool captureMeter(unsigned seconds) {
    CaptureMapping m;
    if (!openCapture(m)) {
        std::cerr << "capture SHM unavailable; start fw1814analog44/48 first\n";
        return false;
    }

    constexpr std::size_t kFrames = 256;
    std::vector<float> block(
        kFrames * macfw::fw1814::hal::capture::kInputChannels, 0.0f);
    std::array<float, macfw::fw1814::hal::capture::kInputChannels> peaks{};
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    auto nextPrint = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

    while (std::chrono::steady_clock::now() < end) {
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
                      << " rate=" << m.ring->sampleRate.load(std::memory_order_relaxed)
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
              << "  " << argv0 << " --init [44100|48000]\n"
              << "  " << argv0 << " --tone <1..4> [frequency-hz]\n"
              << "  " << argv0 << " --capture-meter [seconds]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 64;
    }
    const std::string arg = argv[1];
    if (arg == "--init" && (argc == 2 || argc == 3)) {
        try {
            const std::uint32_t rate = argc == 3
                ? static_cast<std::uint32_t>(std::stoul(argv[2]))
                : 48000u;
            return initPlayback(rate) ? 0 : 1;
        } catch (...) {
            usage(argv[0]);
            return 64;
        }
    }
    if (arg == "--tone" && (argc == 3 || argc == 4)) {
        try {
            const unsigned output = static_cast<unsigned>(std::stoul(argv[2]));
            const double frequency = argc == 4 ? std::stod(argv[3]) : 500.0;
            return tone(output, frequency) ? 0 : 1;
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
