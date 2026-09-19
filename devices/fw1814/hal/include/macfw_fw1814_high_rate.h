#pragma once

#include <sys/stat.h>

namespace macfw::fw1814::experimental {

constexpr const char* kEnable96Path =
    "/Library/Application Support/macfw/fw1814/enable-96-experimental";
constexpr const char* kEnable88Path =
    "/Library/Application Support/macfw/fw1814/enable-88-experimental";
constexpr const char* kEnable176Path =
    "/Library/Application Support/macfw/fw1814/enable-176-experimental";

inline bool enabled88() {
    struct stat st{};
    return stat(kEnable88Path, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == 0 && (st.st_mode & 0022) == 0;
}

inline bool enabled96() {
    struct stat st{};
    return stat(kEnable96Path, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == 0 && (st.st_mode & 0022) == 0;
}

inline bool enabled176() {
    struct stat st{};
    return stat(kEnable176Path, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == 0 && (st.st_mode & 0022) == 0;
}

} // namespace macfw::fw1814::experimental
