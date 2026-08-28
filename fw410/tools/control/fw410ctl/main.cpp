#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr const char* kSocketPath = "/tmp/macfw-fw410-control.sock";
constexpr std::array<const char*,5> kHeadphoneMixerLabels={"1/2","3/4","5/6","7/8","9/10"};
constexpr std::array<const char*,5> kOutputLabels={"Analog 1/2","Analog 3/4","Analog 5/6","Analog 7/8","S/PDIF L/R"};
constexpr std::array<const char*,5> kOutputArgs={"1/2","3/4","5/6","7/8","spdif"};
constexpr std::array<const char*,7> kMainMixerSourceArgs={"analog","spdif","1/2","3/4","5/6","7/8","9/10"};
constexpr std::array<const char*,7> kMainMixerSourceLabels={"Analog In 1/2","S/PDIF In L/R","Software Return 1/2","Software Return 3/4","Software Return 5/6","Software Return 7/8","Software Return 9/10"};
constexpr std::array<const char*,5> kMainMixerDestinationArgs={"1/2","3/4","5/6","7/8","9/10"};
constexpr std::array<const char*,5> kMainMixerDestinationLabels={"Mixer Out 1/2","Mixer Out 3/4","Mixer Out 5/6","Mixer Out 7/8","Mixer Out 9/10"};

int usage(){std::cerr<<"usage:\n  fw410ctl output-state get\n  fw410ctl output-source get 1/2|3/4|5/6|7/8|spdif\n  fw410ctl output-source set 1/2|3/4|5/6|7/8|spdif mixer|aux\n  fw410ctl output-volume get 1/2|3/4|5/6|7/8|spdif\n  fw410ctl output-volume set 1/2|3/4|5/6|7/8|spdif <dB|-inf> [<right-dB|-inf>]\n  fw410ctl mixer-route get analog|spdif|1/2|3/4|5/6|7/8|9/10 1/2|3/4|5/6|7/8|9/10\n  fw410ctl headphone-source get\n  fw410ctl headphone-source set mixer|aux\n  fw410ctl headphone-volume get\n  fw410ctl headphone-volume set <dB|-inf> [<right-dB|-inf>]\n  fw410ctl headphone-mixer get\n  fw410ctl headphone-mixer set 1/2|3/4|5/6|7/8|9/10 on|off\n  fw410ctl aux-stream12-volume get\n  fw410ctl aux-stream12-volume set <dB|-inf> [<right-dB|-inf>]\n  fw410ctl aux-output-volume get\n  fw410ctl aux-output-volume set <dB|-inf> [<right-dB|-inf>]\n\nvolume range: -128..0 dB in 1 dB steps; -inf uses AV/C negative infinity\n";return 64;}

bool transact(const std::string&c,std::string&r){int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0){std::cerr<<"fw410ctl: socket: "<<std::strerror(errno)<<'\n';return false;}sockaddr_un a{};a.sun_family=AF_UNIX;std::strncpy(a.sun_path,kSocketPath,sizeof(a.sun_path)-1);if(connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))!=0){std::cerr<<"fw410ctl: cannot connect to "<<kSocketPath<<": "<<std::strerror(errno)<<'\n';close(fd);return false;}std::string w=c+"\n";const char*p=w.data();std::size_t left=w.size();while(left){ssize_t n=send(fd,p,left,0);if(n<=0){close(fd);return false;}p+=n;left-=static_cast<std::size_t>(n);}r.clear();char b[256];while(r.find('\n')==std::string::npos){ssize_t n=recv(fd,b,sizeof(b),0);if(n>0){r.append(b,static_cast<std::size_t>(n));if(r.size()>1024)break;}else break;}close(fd);return !r.empty();}
bool getPayload(const std::string&c,std::string&p){std::string r;if(!transact(c,r))return false;if(!r.empty()&&r.back()=='\n')r.pop_back();if(r.rfind("OK ",0)!=0){std::cerr<<"fw410ctl: "<<r<<'\n';return false;}p=r.substr(3);return true;}
bool dbToRaw(const std::string&s,int&r){if(s=="-inf"||s=="mute"){r=-32768;return true;}char*e=nullptr;errno=0;long d=std::strtol(s.c_str(),&e,10);if(errno||!e||*e!='\0'||d< -128||d>0)return false;r=static_cast<int>(d*0x100);return true;}
std::string rawToDb(int r){if(r==-32768)return "-inf";if((r%0x100)==0)return std::to_string(r/0x100)+" dB";std::ostringstream o;o<<r<<" raw ("<<(static_cast<double>(r)/256.0)<<" dB)";return o.str();}
std::string outputSourceName(int s){return s==0?"mixer":s==1?"aux":"unknown";}
std::string spdifConnectorName(int s){return s==0?"coaxial":s==1?"optical":"unknown";}

template<std::size_t N> int indexOf(const std::string&s,const std::array<const char*,N>&a){for(std::size_t i=0;i<N;++i)if(s==a[i])return static_cast<int>(i);return -1;}
int outputIndex(const std::string&s){return indexOf(s,kOutputArgs);}
int mixerIndex(const std::string&s){return indexOf(s,kHeadphoneMixerLabels);}

bool parseOutput(const std::string&p,int&s,int&l,int&r){std::istringstream in(p);std::string x;return static_cast<bool>(in>>s>>l>>r)&&!(in>>x);}
int printOutputState(){std::cout<<"FW410 physical output state (read-only):\n";for(std::size_t i=0;i<5;++i){std::string p;if(!getPayload("OUTPUT_PAIR GET "+std::to_string(i),p))return 1;int s=-1,l=0,r=0;if(!parseOutput(p,s,l,r))return 1;std::cout<<"  "<<kOutputLabels[i]<<":\n    source: "<<outputSourceName(s)<<" ("<<s<<")\n    left:   "<<rawToDb(l)<<" (raw "<<l<<")\n    right:  "<<rawToDb(r)<<" (raw "<<r<<")\n";}std::string p;if(!getPayload("SPDIF_CONNECTOR GET",p))return 1;int c=-1;std::istringstream in(p);if(!(in>>c))return 1;std::cout<<"  S/PDIF connector: "<<spdifConnectorName(c)<<" ("<<c<<")\n";return 0;}

int outputSourceCommand(const std::string&a,int argc,char**argv){if((a=="get"&&argc!=4)||(a=="set"&&argc!=5))return usage();int i=outputIndex(argv[3]);if(i<0)return usage();if(a=="get"){std::string p;if(!getPayload("OUTPUT_PAIR GET "+std::to_string(i),p))return 1;int s=-1,l=0,r=0;if(!parseOutput(p,s,l,r))return 1;std::cout<<kOutputLabels[i]<<": "<<outputSourceName(s)<<" ("<<s<<")\n";return 0;}int s=std::string(argv[4])=="mixer"?0:std::string(argv[4])=="aux"?1:-1;if(s<0)return usage();std::string p;if(!getPayload("OUTPUT_PAIR SET_SOURCE "+std::to_string(i)+" "+std::to_string(s),p))return 1;std::cout<<kOutputLabels[i]<<": "<<outputSourceName(s)<<" ("<<s<<")\n";return 0;}
int outputVolumeCommand(const std::string&a,int argc,char**argv){if((a=="get"&&argc!=4)||(a=="set"&&(argc!=5&&argc!=6)))return usage();int i=outputIndex(argv[3]);if(i<0)return usage();if(a=="get"){std::string p;if(!getPayload("OUTPUT_PAIR GET "+std::to_string(i),p))return 1;int s=-1,l=0,r=0;if(!parseOutput(p,s,l,r))return 1;std::cout<<kOutputLabels[i]<<"\nleft:  "<<rawToDb(l)<<" (raw "<<l<<")\nright: "<<rawToDb(r)<<" (raw "<<r<<")\n";return 0;}int l=0,r=0;if(!dbToRaw(argv[4],l))return usage();if(argc==6){if(!dbToRaw(argv[5],r))return usage();}else r=l;std::string p;if(!getPayload("OUTPUT_PAIR SET_LEVEL "+std::to_string(i)+" "+std::to_string(l)+" "+std::to_string(r),p))return 1;std::istringstream in(p);int vi=-1,vl=0,vr=0;if(!(in>>vi>>vl>>vr)||vi!=i||vl!=l||vr!=r){std::cerr<<"fw410ctl: invalid output-level verification: "<<p<<'\n';return 1;}std::cout<<kOutputLabels[i]<<"\nleft:  "<<rawToDb(vl)<<" (raw "<<vl<<")\nright: "<<rawToDb(vr)<<" (raw "<<vr<<")\n";return 0;}

int mainMixerRouteCommand(const std::string&a,int argc,char**argv){if(a!="get"||argc!=5)return usage();int src=indexOf(std::string(argv[3]),kMainMixerSourceArgs);int dst=indexOf(std::string(argv[4]),kMainMixerDestinationArgs);if(src<0||dst<0)return usage();std::string p;if(!getPayload("MAIN_MIXER_ROUTE GET "+std::to_string(src)+" "+std::to_string(dst),p))return 1;std::istringstream in(p);int rs=-1,rd=-1,v=-1;std::string x;if(!(in>>rs>>rd>>v)||(in>>x)||rs!=src||rd!=dst||(v!=0&&v!=1)){std::cerr<<"fw410ctl: invalid main-mixer response: "<<p<<'\n';return 1;}std::cout<<kMainMixerSourceLabels[static_cast<std::size_t>(src)]<<" -> "<<kMainMixerDestinationLabels[static_cast<std::size_t>(dst)]<<": "<<(v?"on":"off")<<" ("<<v<<")\n";return 0;}

bool parseStereo(const std::string&p,int&l,int&r){std::istringstream in(p);std::string x;return static_cast<bool>(in>>l>>r)&&!(in>>x);}
int printLevel(const std::string&p){int l=0,r=0;if(!parseStereo(p,l,r)){std::cout<<p<<'\n';return 0;}std::cout<<"left:  "<<rawToDb(l)<<" (raw "<<l<<")\nright: "<<rawToDb(r)<<" (raw "<<r<<")\n";return 0;}
bool buildVolume(const std::string&w,int argc,char**argv,std::string&c){std::string a=argv[2];if(a=="get"&&argc==3){c=w+" GET";return true;}if(a!="set"||(argc!=4&&argc!=5))return false;int l=0,r=0;if(!dbToRaw(argv[3],l))return false;if(argc==5){if(!dbToRaw(argv[4],r))return false;}else r=l;c=w+" SET "+std::to_string(l)+" "+std::to_string(r);return true;}
int printMixer(const std::string&p){std::istringstream in(p);for(const char*l:kHeadphoneMixerLabels){int v=-1;if(!(in>>v))return 1;std::cout<<l<<": "<<(v?"on":"off")<<'\n';}return 0;}
}

int main(int argc,char**argv){
    if(argc<3)return usage();
    std::string control=argv[1],action=argv[2];
    if(control=="output-state")return action=="get"&&argc==3?printOutputState():usage();
    if(control=="output-source")return (action=="get"||action=="set")?outputSourceCommand(action,argc,argv):usage();
    if(control=="output-volume")return (action=="get"||action=="set")?outputVolumeCommand(action,argc,argv):usage();
    if(control=="mixer-route")return mainMixerRouteCommand(action,argc,argv);
    std::string command,display;bool level=false,source=false,mixer=false;
    if(control=="headphone-source"){
        if(action=="get"&&argc==3){command="HEADPHONE_SOURCE GET";source=true;}
        else if(action=="set"&&argc==4){std::string v=argv[3];if(v=="mixer")v="0";else if(v=="aux")v="1";if(v!="0"&&v!="1")return usage();command="HEADPHONE_SOURCE SET "+v;source=true;}
        else return usage();
    }else if(control=="headphone-volume"){
        if(!buildVolume("HEADPHONE_VOLUME",argc,argv,command))return usage();level=true;
    }else if(control=="headphone-mixer"){
        if(action=="get"&&argc==3){command="HEADPHONE_MIXER GET";mixer=true;}
        else if(action=="set"&&argc==5){int i=mixerIndex(argv[3]);std::string s=argv[4];if(i<0||(s!="on"&&s!="off"))return usage();command="HEADPHONE_MIXER SET "+std::to_string(i)+" "+(s=="on"?"1":"0");display=std::string(argv[3])+": "+s;}
        else return usage();
    }else if(control=="aux-stream12-volume"){
        if(!buildVolume("AUX_STREAM12_VOLUME",argc,argv,command))return usage();level=true;
    }else if(control=="aux-output-volume"){
        if(!buildVolume("AUX_OUTPUT_VOLUME",argc,argv,command))return usage();level=true;
    }else return usage();
    std::string response;if(!transact(command,response))return 1;if(!response.empty()&&response.back()=='\n')response.pop_back();if(response.rfind("OK ",0)!=0){std::cerr<<"fw410ctl: "<<response<<'\n';return 1;}std::string p=response.substr(3);if(level)return printLevel(p);if(source){std::cout<<(p=="0"?"mixer (0)":p=="1"?"aux (1)":p)<<'\n';return 0;}if(mixer)return printMixer(p);if(!display.empty()){std::cout<<display<<'\n';return 0;}std::cout<<p<<'\n';return 0;
}
