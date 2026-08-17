#include "../include/macfw_hal_shm.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

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
    std::printf("HAL shared ring: %s\n", macfw::hal::valid(*ring) ? "PASS" : "INVALID");
    std::printf("    sample rate:    %u Hz\n", ring->sampleRate.load(std::memory_order_acquire));
    std::printf("    write frame:    %llu\n", static_cast<unsigned long long>(ring->writeFrame.load(std::memory_order_acquire)));
    std::printf("    read frame:     %llu\n", static_cast<unsigned long long>(ring->readFrame.load(std::memory_order_acquire)));
    std::printf("    available:      %llu frames\n", static_cast<unsigned long long>(macfw::hal::availableFrames(*ring)));
    std::printf("    dropped frames: %llu\n", static_cast<unsigned long long>(ring->droppedFrames.load(std::memory_order_acquire)));

    munmap(p, sizeof(*ring));
    close(fd);
    return macfw::hal::valid(*ring) ? 0 : 2;
}
