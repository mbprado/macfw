#pragma once

#include <sys/stat.h>

namespace macfw::fw1814::experimental {

constexpr const char* kEnable96Path =
    "/Library/Application Support/macfw/fw1814/enable-96-experimental";

inline bool enabled96() {
    struct stat st{};
    return stat(kEnable96Path, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == 0 && (st.st_mode & 0022) == 0;
}

} // namespace macfw::fw1814::experimental
