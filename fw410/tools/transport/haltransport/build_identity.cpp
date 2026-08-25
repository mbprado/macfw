#include "../../../version.h"

#include <crt_externs.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
struct BuildIdentityBanner {
    BuildIdentityBanner() {
        bool verbose = false;
        const int argc = *_NSGetArgc();
        char** argv = *_NSGetArgv();
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--verbose") == 0 ||
                std::strcmp(argv[i], "--debug") == 0) {
                verbose = true;
                break;
            }
        }
        if (verbose) setenv("MACFW_VERBOSE", "1", 1);

        std::printf("macfw haltransport %s build %s\n", macfw::build::kVersion,
                    macfw::build::kGitSha);
        if (verbose)
            std::printf("verbose transport diagnostics: enabled\n");
    }
};

BuildIdentityBanner gBuildIdentityBanner;
} // namespace
