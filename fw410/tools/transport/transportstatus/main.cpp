#include "macfw_hal_transport_status.h"
#include "macfw_hal_capture_shm.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

using macfw::hal::transport::SharedStatus;
using macfw::hal::transport::State;
using macfw::hal::capture::SharedCaptureRing;

struct Snapshot {
    State state = State::Offline;
    std::uint32_t requestedRate = 0;
    std::uint32_t activeRate = 0;
    std::uint32_t enginePid = 0;
    std::uint64_t transitions = 0;
    std::uint64_t heartbeat = 0;
};

struct CaptureSnapshot {
    bool available = false;
    std::uint32_t sampleRate = 0;
    std::uint32_t active = 0;
    std::uint64_t writeFrame = 0;
    std::uint64_t readFrame = 0;
    std::uint64_t queuedFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t halReadCalls = 0;
    std::uint64_t halRequestedFrames = 0;
    std::uint64_t halFramesFromRing = 0;
    std::uint64_t halZeroFilledFrames = 0;
};

Snapshot snapshot(const SharedStatus& s) {
    Snapshot out;
    out.state = static_cast<State>(s.state.load(std::memory_order_acquire));
    out.requestedRate = s.requestedRate.load(std::memory_order_acquire);
    out.activeRate = s.activeRate.load(std::memory_order_acquire);
    out.enginePid = s.enginePid.load(std::memory_order_acquire);
    out.transitions = s.transitionSequence.load(std::memory_order_acquire);
    out.heartbeat = s.heartbeatSequence.load(std::memory_order_acquire);
    return out;
}

CaptureSnapshot captureSnapshot(const SharedCaptureRing* ring) {
    CaptureSnapshot out;
    if (!ring || !macfw::hal::capture::valid(*ring)) return out;

    out.available = true;
    out.sampleRate = ring->sampleRate.load(std::memory_order_acquire);
    out.active = ring->active.load(std::memory_order_acquire);
    out.writeFrame = ring->writeFrame.load(std::memory_order_acquire);
    out.readFrame = ring->readFrame.load(std::memory_order_acquire);
    out.queuedFrames = out.writeFrame >= out.readFrame ? out.writeFrame - out.readFrame : 0;
    out.droppedFrames = ring->droppedFrames.load(std::memory_order_acquire);
    out.halReadCalls = ring->halReadCalls.load(std::memory_order_acquire);
    out.halRequestedFrames = ring->halRequestedFrames.load(std::memory_order_acquire);
    out.halFramesFromRing = ring->halFramesFromRing.load(std::memory_order_acquire);
    out.halZeroFilledFrames = ring->halZeroFilledFrames.load(std::memory_order_acquire);
    return out;
}

void print(const Snapshot& s) {
    std::printf("transport state: %s\n", macfw::hal::transport::stateName(s.state));
    std::printf("    requested rate: %u Hz\n", s.requestedRate);
    std::printf("    active rate:    %u Hz\n", s.activeRate);
    std::printf("    engine pid:     %u\n", s.enginePid);
    std::printf("    transitions:    %llu\n", static_cast<unsigned long long>(s.transitions));
    std::printf("    heartbeat:      %llu\n", static_cast<unsigned long long>(s.heartbeat));
}

void printCapture(const CaptureSnapshot& s) {
    if (!s.available) {
        std::printf("capture state: unavailable\n");
        return;
    }

    const double queueMs = s.sampleRate != 0
        ? (1000.0 * static_cast<double>(s.queuedFrames)) / static_cast<double>(s.sampleRate)
        : 0.0;
    std::printf("capture state: %s\n", s.active ? "active" : "prefill");
    std::printf("    capture rate:   %u Hz\n", s.sampleRate);
    std::printf("    queued frames:  %llu (%.2f ms)\n",
                static_cast<unsigned long long>(s.queuedFrames), queueMs);
    std::printf("    write frame:    %llu\n", static_cast<unsigned long long>(s.writeFrame));
    std::printf("    read frame:     %llu\n", static_cast<unsigned long long>(s.readFrame));
    std::printf("    dropped frames: %llu\n", static_cast<unsigned long long>(s.droppedFrames));
    std::printf("    hal read calls: %llu\n", static_cast<unsigned long long>(s.halReadCalls));
    std::printf("    hal requested:  %llu\n", static_cast<unsigned long long>(s.halRequestedFrames));
    std::printf("    hal from ring:  %llu\n", static_cast<unsigned long long>(s.halFramesFromRing));
    std::printf("    hal zero fill:  %llu\n", static_cast<unsigned long long>(s.halZeroFilledFrames));
}

const SharedCaptureRing* mapCapture(int& fdOut) {
    fdOut = shm_open(macfw::hal::capture::kShmName, O_RDONLY, 0);
    if (fdOut < 0) return nullptr;

    struct stat st = {};
    if (fstat(fdOut, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(SharedCaptureRing))) {
        close(fdOut);
        fdOut = -1;
        return nullptr;
    }

    void* p = mmap(nullptr, sizeof(SharedCaptureRing), PROT_READ, MAP_SHARED, fdOut, 0);
    close(fdOut);
    fdOut = -1;
    if (p == MAP_FAILED) return nullptr;
    return static_cast<const SharedCaptureRing*>(p);
}

} // namespace

int main(int argc, char** argv) {
    const bool watch = argc > 1 && std::strcmp(argv[1], "--watch") == 0;

    const int fd = shm_open(macfw::hal::transport::kShmName, O_RDONLY, 0);
    if (fd < 0) {
        std::fprintf(stderr, "transport status shared memory unavailable: %s\n",
                     std::strerror(errno));
        return 1;
    }

    struct stat st = {};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(SharedStatus))) {
        std::fprintf(stderr, "transport status shared memory has unexpected size\n");
        close(fd);
        return 1;
    }

    void* p = mmap(nullptr, sizeof(SharedStatus), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        std::fprintf(stderr, "transport status mmap failed: %s\n", std::strerror(errno));
        return 1;
    }

    const auto* status = static_cast<const SharedStatus*>(p);
    if (!macfw::hal::transport::valid(*status)) {
        std::fprintf(stderr, "transport status ABI mismatch\n");
        munmap(p, sizeof(SharedStatus));
        return 1;
    }

    int captureFd = -1;
    const SharedCaptureRing* capture = mapCapture(captureFd);

    if (!watch) {
        print(snapshot(*status));
        printCapture(captureSnapshot(capture));
        if (capture) munmap(const_cast<SharedCaptureRing*>(capture), sizeof(SharedCaptureRing));
        munmap(p, sizeof(SharedStatus));
        return 0;
    }

    while (true) {
        print(snapshot(*status));
        printCapture(captureSnapshot(capture));
        std::printf("\n");
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
