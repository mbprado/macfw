#pragma once

namespace macfw::fw1814::experimental {

constexpr const char* kEnable96Path =
    "/Library/Application Support/macfw/fw1814/enable-96-experimental";
constexpr const char* kEnable88Path =
    "/Library/Application Support/macfw/fw1814/enable-88-experimental";
constexpr const char* kEnable176Path =
    "/Library/Application Support/macfw/fw1814/enable-176-experimental";
constexpr const char* kEnable192Path =
    "/Library/Application Support/macfw/fw1814/enable-192-experimental";

inline bool enabled88() {
    return true;
}

inline bool enabled96() {
    return true;
}

inline bool enabled176() {
    return true;
}

inline bool enabled192() {
    return true;
}

} // namespace macfw::fw1814::experimental
