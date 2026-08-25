#include "macfw_hal_transport_status.h"

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

struct Snapshot {
    State state = State::Offline;
    std::uint32_t requestedRate = 0;
    std::uint32_t activeRate = 0;
    std::uint32_t enginePid = 0;
    std::uint64_t transitions = 0;
    std::uint64_t heartbeat = 0;
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

void print(const Snapshot& s) {
    std::printf("transport state: %s\n", macfw::hal::transport::stateName(s.state));
    std::printf("    requested rate: %u Hz\n", s.requestedRate);
    std::printf("    active rate:    %u Hz\n", s.activeRate);
    std::printf("    engine pid:     %u\n", s.enginePid);
    std::printf("    transitions:    %llu\n", static_cast<unsigned long long>(s.transitions));
    std::printf("    heartbeat:      %llu\n", static_cast<unsigned long long>(s.heartbeat));
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

    if (!watch) {
        print(snapshot(*status));
        munmap(p, sizeof(SharedStatus));
        return 0;
    }

    std::uint64_t lastTransition = ~std::uint64_t{0};
    while (true) {
        const Snapshot current = snapshot(*status);
        if (current.transitions != lastTransition) {
            print(current);
            std::printf("\n");
            std::fflush(stdout);
            lastTransition = current.transitions;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
