#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <libgen.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

int pcmstream44100_inner_main(int argc, char** argv);

namespace {
bool hasExecuteFlag(int argc, char** argv) {
    for (int i=1;i<argc;++i) if (std::strcmp(argv[i],"--execute")==0) return true;
    return false;
}
std::string executableDirectory(const char* argv0) {
    char resolved[PATH_MAX] = {};
    if (argv0 && realpath(argv0,resolved)) {
        char copy[PATH_MAX] = {}; std::strncpy(copy,resolved,sizeof(copy)-1); return dirname(copy);
    }
    return ".";
}
int runRateProbe(const std::string& path,const char* rate) {
    const pid_t pid=fork();
    if (pid<0) return 127;
    if (pid==0) { execl(path.c_str(),path.c_str(),rate,"--execute","--keep",static_cast<char*>(nullptr)); _exit(127); }
    int status=0; while (waitpid(pid,&status,0)<0) { if (errno==EINTR) continue; return 127; }
    return WIFEXITED(status)?WEXITSTATUS(status):128;
}
}
int main(int argc,char** argv) {
    if (!hasExecuteFlag(argc,argv)) return pcmstream44100_inner_main(argc,argv);
    const std::string here=executableDirectory(argc>0?argv[0]:nullptr);
    const std::string rateProbe=here+"/../../control/rateprobe/rateprobe";
    std::printf("initial FW410 rate setup: 44100 Hz\n");
    if (runRateProbe(rateProbe,"44100")!=0) return 1;
    const int result=pcmstream44100_inner_main(argc,argv);
    std::printf("restoring FW410 rate: 48000 Hz\n");
    const int restore=runRateProbe(rateProbe,"48000");
    return result!=0?result:(restore==0?0:1);
}
