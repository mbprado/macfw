#include "../../../hal/include/macfw_hal_transport_status.h"

#include <cerrno>
#include <cstdio>
#include <sys/mman.h>

int main() {
    const char* name = macfw::hal::transport::kShmName;
    if (shm_unlink(name) == 0) {
        std::printf("removed transport status SHM: %s\n", name);
        return 0;
    }
    if (errno == ENOENT) {
        std::printf("transport status SHM already absent: %s\n", name);
        return 0;
    }
    std::perror("transport status shm_unlink");
    return 1;
}
