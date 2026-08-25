#pragma once

#include <cerrno>
#include <cstdlib>
#include <unistd.h>

namespace macfw::transport {

// Native bridge processes inherit a write end from haltransport through this
// environment variable.  Standalone bridge execution has no variable set and
// therefore remains unchanged.
inline void signalEngineReady() {
    const char* value = std::getenv("MACFW_ENGINE_READY_FD");
    if (!value || !*value) return;

    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (!end || *end != '\0' || parsed < 0) return;

    const int fd = static_cast<int>(parsed);
    const unsigned char ready = 1;
    ssize_t result;
    do {
        result = write(fd, &ready, sizeof(ready));
    } while (result < 0 && errno == EINTR);

    close(fd);
    unsetenv("MACFW_ENGINE_READY_FD");
}

} // namespace macfw::transport
