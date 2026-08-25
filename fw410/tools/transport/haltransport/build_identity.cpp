#include "../../../version.h"

#include <cstdio>

namespace {
struct BuildIdentityBanner {
    BuildIdentityBanner() {
        std::printf("macfw haltransport %s build %s\n", macfw::build::kVersion,
                    macfw::build::kGitSha);
    }
};

BuildIdentityBanner gBuildIdentityBanner;
} // namespace
