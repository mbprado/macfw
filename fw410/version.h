#pragma once

#ifndef MACFW_VERSION
#define MACFW_VERSION "0.01.000"
#endif

#ifndef MACFW_GIT_SHA
#define MACFW_GIT_SHA "unknown"
#endif

#define MACFW_BUILD_ID "macfw " MACFW_VERSION " build " MACFW_GIT_SHA

namespace macfw {
namespace build {
inline constexpr const char* kVersion = MACFW_VERSION;
inline constexpr const char* kGitSha = MACFW_GIT_SHA;
inline constexpr const char* kBuildId = MACFW_BUILD_ID;
} // namespace build
} // namespace macfw
