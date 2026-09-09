#pragma once

#include <cerrno>
#include <cstdlib>
#include <unistd.h>

namespace macfw::fw1814::transport {

// The supervisor passes a pipe write descriptor through this environment
// variable. Standalone engine execution has no variable and remains unchanged.
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

} // namespace macfw::fw1814::transport
