#pragma once

#include "full_duplex_fcp_control.h"
#include "fw410_main_mixer_model.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace macfw::transport::duplex {

class Fw410ControlServer {
public:
    static constexpr const char* kSocketPath = "/tmp/macfw-fw410-control.sock";
    static constexpr std::uint8_t kHeadphoneSelector = 0x07;
    static constexpr std::uint8_t kHeadphoneLevel = 0x0f;
    static constexpr std::uint8_t kAuxStream12Level = 0x06;
    static constexpr std::uint8_t kAuxOutputLevel = 0x09;
    static constexpr std::uint8_t kHeadphoneMixerBlock = 0x07;
    static constexpr std::uint8_t kHeadphoneMixerInputPlug = 0x00;
    static constexpr std::uint8_t kHeadphoneMixerOutputChannel = 0x01;
    static constexpr std::array<std::uint8_t, 5> kHeadphoneMixerInputChannels = {0x01,0x03,0x05,0x07,0x09};
    static constexpr std::array<std::uint8_t, 5> kOutputSelectorBlocks = {0x02,0x03,0x04,0x05,0x06};
    static constexpr std::array<std::uint8_t, 5> kOutputLevelBlocks = {0x0a,0x0b,0x0c,0x0d,0x0e};
    static constexpr std::uint8_t kSpdifConnectorSelector = 0x01;

    // FFADO's FW410 mixer model maps the seven main source-strip faders to
    // these feature blocks/channels.  Keep this probe read-only until the
    // hardware values have been compared with the original control panel.
    struct MainStripLevelAddress { std::uint8_t functionBlock; std::uint8_t leftChannel; std::uint8_t rightChannel; };
    static constexpr std::array<MainStripLevelAddress, 7> kMainStripLevelAddresses = {{
        {0x03,0x01,0x02}, // Analog 1/2
        {0x04,0x01,0x02}, // S/PDIF 1/2
        {0x02,0x01,0x02}, // SW return 1/2
        {0x01,0x01,0x02}, // SW return 3/4
        {0x01,0x03,0x04}, // SW return 5/6
        {0x01,0x05,0x06}, // SW return 7/8
        {0x01,0x07,0x08}, // SW return 9/10
    }};

    ~Fw410ControlServer(){reset();}
    bool start(Fw410FcpControl& fcp){reset();fcp_=&fcp;listenFd_=socket(AF_UNIX,SOCK_STREAM,0);if(listenFd_<0)return false;int flags=fcntl(listenFd_,F_GETFL,0);if(flags>=0)fcntl(listenFd_,F_SETFL,flags|O_NONBLOCK);sockaddr_un a{};a.sun_family=AF_UNIX;if(std::strlen(kSocketPath)>=sizeof(a.sun_path)){reset();return false;}std::strncpy(a.sun_path,kSocketPath,sizeof(a.sun_path)-1);unlink(kSocketPath);if(bind(listenFd_,reinterpret_cast<sockaddr*>(&a),sizeof(a))!=0){reset();return false;}chmod(kSocketPath,0666);if(listen(listenFd_,4)!=0){reset();return false;}std::printf("FW410 control socket: %s\n",kSocketPath);return true;}
    void reset(){if(clientFd_>=0)close(clientFd_);clientFd_=-1;request_.clear();if(listenFd_>=0)close(listenFd_);listenFd_=-1;unlink(kSocketPath);fcp_=nullptr;mainMixerHardwareInitialized_=false;}
    void service(){if(listenFd_<0||!fcp_)return;if(clientFd_<0){clientFd_=accept(listenFd_,nullptr,nullptr);if(clientFd_>=0){int flags=fcntl(clientFd_,F_GETFL,0);if(flags>=0)fcntl(clientFd_,F_SETFL,flags|O_NONBLOCK);request_.clear();}else if(errno!=EAGAIN&&errno!=EWOULDBLOCK)return;}if(clientFd_<0)return;char b[256];ssize_t n=recv(clientFd_,b,sizeof(b),0);if(n>0){request_.append(b,static_cast<std::size_t>(n));if(request_.size()>1024){reply("ERR request-too-long\n");finishClient();return;}auto nl=request_.find('\n');if(nl!=std::string::npos){handle(request_.substr(0,nl));finishClient();}}else if(n==0||(errno!=EAGAIN&&errno!=EWOULDBLOCK))finishClient();}
private:
    void finishClient(){if(clientFd_>=0)close(clientFd_);clientFd_=-1;request_.clear();}
    void reply(const std::string&t){if(clientFd_<0)return;const char*p=t.data();std::size_t left=t.size();while(left){ssize_t n=send(clientFd_,p,left,0);if(n<=0)break;p+=n;left-=static_cast<std::size_t>(n);}}
    bool readStereoLevel(std::uint8_t fb,std::int16_t&l,std::int16_t&r){return fcp_->readLevel(fb,1,l)&&fcp_->readLevel(fb,2,r);}
    bool readMainStripLevel(std::size_t i,std::int16_t&l,std::int16_t&r){if(i>=kMainStripLevelAddresses.size())return false;const auto&a=kMainStripLevelAddresses[i];return fcp_->readLevel(a.functionBlock,a.leftChannel,l)&&fcp_->readLevel(a.functionBlock,a.rightChannel,r);}
    bool writeStereoLevel(std::uint8_t fb,std::int16_t l,std::int16_t r){if(!fcp_->writeLevel(fb,1,l)||!fcp_->writeLevel(fb,2,r))return false;std::int16_t vl=0,vr=0;return readStereoLevel(fb,vl,vr)&&vl==l&&vr==r;}
    static bool parseRawLevel(const std::string&s,std::int16_t&v){char*e=nullptr;errno=0;long p=std::strtol(s.c_str(),&e,10);if(errno||!e||*e!='\0')return false;if(p==-32768){v=static_cast<std::int16_t>(p);return true;}if(p<-32768||p>0||(p%0x100)!=0)return false;v=static_cast<std::int16_t>(p);return true;}
    void handleLevel(const std::string&c,const std::string&p,std::uint8_t fb){if(c==p+" GET"){std::int16_t l=0,r=0;if(!readStereoLevel(fb,l,r)){reply("ERR fcp-read-failed\n");return;}reply("OK "+std::to_string(l)+" "+std::to_string(r)+"\n");return;}std::string sp=p+" SET ";if(c.rfind(sp,0)!=0)return;std::istringstream in(c.substr(sp.size()));std::string ls,rs,x;if(!(in>>ls>>rs)||(in>>x)){reply("ERR invalid-level\n");return;}std::int16_t l=0,r=0;if(!parseRawLevel(ls,l)||!parseRawLevel(rs,r)){reply("ERR invalid-level\n");return;}if(!writeStereoLevel(fb,l,r)){reply("ERR fcp-write-or-verify-failed\n");return;}reply("OK "+std::to_string(l)+" "+std::to_string(r)+"\n");}
    void handleMainStripLevel(const std::string&c){const std::string p="MAIN_STRIP_LEVEL GET ";if(c.rfind(p,0)!=0){reply("ERR read-only-control\n");return;}std::istringstream in(c.substr(p.size()));unsigned i=0;std::string x;if(!(in>>i)||(in>>x)||i>=kMainStripLevelAddresses.size()){reply("ERR invalid-main-strip\n");return;}std::int16_t l=0,r=0;if(!readMainStripLevel(i,l,r)){reply("ERR fcp-read-failed\n");return;}reply("OK "+std::to_string(i)+" "+std::to_string(l)+" "+std::to_string(r)+"\n");}
    bool readHeadphoneMixer(std::array<bool,5>&s){for(std::size_t i=0;i<s.size();++i)if(!fcp_->readProcessingMixer(kHeadphoneMixerBlock,kHeadphoneMixerInputPlug,kHeadphoneMixerInputChannels[i],kHeadphoneMixerOutputChannel,s[i]))return false;return true;}
    bool writeHeadphoneMixer(std::size_t i,bool e){if(i>=5||!fcp_->writeProcessingMixer(kHeadphoneMixerBlock,kHeadphoneMixerInputPlug,kHeadphoneMixerInputChannels[i],kHeadphoneMixerOutputChannel,e))return false;bool v=false;return fcp_->readProcessingMixer(kHeadphoneMixerBlock,kHeadphoneMixerInputPlug,kHeadphoneMixerInputChannels[i],kHeadphoneMixerOutputChannel,v)&&v==e;}
    void handleHeadphoneMixer(const std::string&c){if(c=="HEADPHONE_MIXER GET"){std::array<bool,5>s{};if(!readHeadphoneMixer(s)){reply("ERR fcp-read-failed\n");return;}std::string o="OK";for(bool e:s)o+=e?" 1":" 0";reply(o+"\n");return;}std::string p="HEADPHONE_MIXER SET ";if(c.rfind(p,0)!=0){reply("ERR unknown-command\n");return;}std::istringstream in(c.substr(p.size()));unsigned i=0,v=0;std::string x;if(!(in>>i>>v)||(in>>x)||i>=5||v>1){reply("ERR invalid-headphone-mixer\n");return;}if(!writeHeadphoneMixer(i,v!=0)){reply("ERR fcp-write-or-verify-failed\n");return;}reply("OK "+std::to_string(i)+" "+std::to_string(v)+"\n");}

    bool writeFullMainMixer(const Fw410MainMixerModel& desired,std::size_t& writes,std::size_t& failedSrc,std::size_t& failedDst){writes=0;for(std::size_t dst=0;dst<Fw410MainMixerModel::kDestinationCount;++dst){const auto destination=static_cast<Fw410MainMixerModel::Destination>(dst);const auto outputChannel=Fw410MainMixerModel::kAvcDestinationChannels[dst];for(std::size_t src=0;src<Fw410MainMixerModel::kSourceCount;++src){const auto source=static_cast<Fw410MainMixerModel::Source>(src);const auto& avc=Fw410MainMixerModel::kAvcSources[src];const bool enabled=desired.route(source,destination);if(!fcp_->writeProcessingMixer(Fw410MainMixerModel::kDestinationFunctionBlock,avc.functionBlock,avc.channel,outputChannel,enabled)){failedSrc=src;failedDst=dst;return false;}++writes;}}return true;}
    bool initializeMainMixer(){if(mainMixerHardwareInitialized_)return true;Fw410MainMixerModel desired;desired.loadMacfwRemappedExperimentPreset();std::size_t writes=0,failedSrc=0,failedDst=0;if(!writeFullMainMixer(desired,writes,failedSrc,failedDst)){std::fprintf(stderr,"FW410 main mixer init failed after %zu writes src=%zu dst=%zu\n",writes,failedSrc,failedDst);mainMixerHardwareInitialized_=false;return false;}mainMixerModel_=desired;mainMixerHardwareInitialized_=true;std::printf("FW410 main mixer initialized with macfw routing (35 CONTROL writes)\n");return true;}
    bool writeCachedMainMixerRoute(std::size_t src,std::size_t dst,bool enabled){if(!initializeMainMixer())return false;const auto source=static_cast<Fw410MainMixerModel::Source>(src);const auto destination=static_cast<Fw410MainMixerModel::Destination>(dst);if(mainMixerModel_.route(source,destination)==enabled)return true;const auto& avc=Fw410MainMixerModel::kAvcSources[src];const auto outputChannel=Fw410MainMixerModel::kAvcDestinationChannels[dst];if(!fcp_->writeProcessingMixer(Fw410MainMixerModel::kDestinationFunctionBlock,avc.functionBlock,avc.channel,outputChannel,enabled))return false;mainMixerModel_.setRoute(source,destination,enabled);return true;}
    void handleMainMixer(const std::string&c){
        if(c=="MAIN_MIXER INIT"){if(!initializeMainMixer()){reply("ERR main-mixer-init-failed\n");return;}reply("OK initialized\n");return;}
        if(!initializeMainMixer()){reply("ERR main-mixer-init-failed\n");return;}
        if(c=="MAIN_MIXER GET"){std::string o="OK";for(const auto& row:mainMixerModel_.routes())for(bool enabled:row)o+=enabled?" 1":" 0";reply(o+"\n");return;}
        const std::string rg="MAIN_MIXER ROUTE GET ";
        if(c.rfind(rg,0)==0){std::istringstream in(c.substr(rg.size()));unsigned src=0,dst=0;std::string x;if(!(in>>src>>dst)||(in>>x)||src>=Fw410MainMixerModel::kSourceCount||dst>=Fw410MainMixerModel::kDestinationCount){reply("ERR invalid-main-mixer-route\n");return;}const auto source=static_cast<Fw410MainMixerModel::Source>(src);const auto destination=static_cast<Fw410MainMixerModel::Destination>(dst);reply("OK "+std::to_string(src)+" "+std::to_string(dst)+" "+(mainMixerModel_.route(source,destination)?"1":"0")+"\n");return;}
        const std::string rs="MAIN_MIXER ROUTE SET ";
        if(c.rfind(rs,0)==0){std::istringstream in(c.substr(rs.size()));unsigned src=0,dst=0,value=0;std::string x;if(!(in>>src>>dst>>value)||(in>>x)||src>=Fw410MainMixerModel::kSourceCount||dst>=Fw410MainMixerModel::kDestinationCount||value>1){reply("ERR invalid-main-mixer-route\n");return;}if(!writeCachedMainMixerRoute(src,dst,value!=0)){reply("ERR fcp-write-failed\n");return;}reply("OK "+std::to_string(src)+" "+std::to_string(dst)+" "+std::to_string(value)+"\n");return;}
        reply("ERR unknown-command\n");
    }

    void handleMainMixerModel(const std::string&c){
        if(c=="MAIN_MIXER_MODEL GET"){std::string o="OK";for(const auto& row:mainMixerModel_.routes())for(bool enabled:row)o+=enabled?" 1":" 0";reply(o+"\n");return;}
        if(c=="MAIN_MIXER_MODEL LOAD_ORIGINAL"){mainMixerModel_.loadOriginalIdentityPreset();mainMixerHardwareInitialized_=false;reply("OK software-only original-identity-preset\n");return;}
        if(c=="MAIN_MIXER_MODEL LOAD_MACFW"){mainMixerModel_.loadMacfwPlaybackPreset();mainMixerHardwareInitialized_=false;reply("OK software-only macfw-playback-preset\n");return;}
        if(c=="MAIN_MIXER_MODEL CLEAR"){mainMixerModel_.clear();mainMixerHardwareInitialized_=false;reply("OK software-only cleared\n");return;}
        const std::string rg="MAIN_MIXER_MODEL ROUTE GET ";
        if(c.rfind(rg,0)==0){std::istringstream in(c.substr(rg.size()));unsigned src=0,dst=0;std::string x;if(!(in>>src>>dst)||(in>>x)||src>=Fw410MainMixerModel::kSourceCount||dst>=Fw410MainMixerModel::kDestinationCount){reply("ERR invalid-main-mixer-route\n");return;}const auto source=static_cast<Fw410MainMixerModel::Source>(src);const auto destination=static_cast<Fw410MainMixerModel::Destination>(dst);reply("OK "+std::to_string(src)+" "+std::to_string(dst)+" "+(mainMixerModel_.route(source,destination)?"1":"0")+"\n");return;}
        const std::string rs="MAIN_MIXER_MODEL ROUTE SET ";
        if(c.rfind(rs,0)==0){std::istringstream in(c.substr(rs.size()));unsigned src=0,dst=0,value=0;std::string x;if(!(in>>src>>dst>>value)||(in>>x)||src>=Fw410MainMixerModel::kSourceCount||dst>=Fw410MainMixerModel::kDestinationCount||value>1){reply("ERR invalid-main-mixer-route\n");return;}const auto source=static_cast<Fw410MainMixerModel::Source>(src);const auto destination=static_cast<Fw410MainMixerModel::Destination>(dst);mainMixerModel_.setRoute(source,destination,value!=0);mainMixerHardwareInitialized_=false;reply("OK software-only "+std::to_string(src)+" "+std::to_string(dst)+" "+std::to_string(value)+"\n");return;}
        if(c=="MAIN_MIXER_MODEL PLAN"){std::string o="OK software-only 35\n";for(std::size_t dst=0;dst<Fw410MainMixerModel::kDestinationCount;++dst){const auto destination=static_cast<Fw410MainMixerModel::Destination>(dst);const auto outCh=Fw410MainMixerModel::kAvcDestinationChannels[dst];for(std::size_t src=0;src<Fw410MainMixerModel::kSourceCount;++src){const auto source=static_cast<Fw410MainMixerModel::Source>(src);const auto& avc=Fw410MainMixerModel::kAvcSources[src];const bool enabled=mainMixerModel_.route(source,destination);o+="src="+std::to_string(src)+" dst="+std::to_string(dst)+" fb="+std::to_string(Fw410MainMixerModel::kDestinationFunctionBlock)+" inputPlug="+std::to_string(avc.functionBlock)+" inputCh="+std::to_string(avc.channel)+" outputCh="+std::to_string(outCh)+" raw="+(enabled?std::string("0x0000"):std::string("0x8000"))+"\n";}}reply(o);return;}
        if(c=="MAIN_MIXER_MODEL TOPOLOGY"){std::string o="OK topology\n";for(std::size_t src=0;src<Fw410MainMixerModel::kSourceCount;++src){const auto& avc=Fw410MainMixerModel::kAvcSources[src];o+="src="+std::to_string(src)+" fb="+std::to_string(avc.functionBlock)+" ch="+std::to_string(avc.channel)+"\n";}reply(o);return;}
        reply("ERR unknown-command\n");
    }
    void handleMainMixerHardware(const std::string&c){
        const std::string routePrefix="MAIN_MIXER_HW ROUTE SET ";
        if(c.rfind(routePrefix,0)==0){if(!mainMixerHardwareInitialized_){reply("ERR main-mixer-not-initialized\n");return;}std::istringstream in(c.substr(routePrefix.size()));unsigned src=0,dst=0,value=0;std::string x;if(!(in>>src>>dst>>value)||(in>>x)||src>=Fw410MainMixerModel::kSourceCount||dst>=Fw410MainMixerModel::kDestinationCount||value>1){reply("ERR invalid-main-mixer-route\n");return;}if(!writeCachedMainMixerRoute(src,dst,value!=0)){reply("ERR fcp-write-failed\n");return;}reply("OK hardware-cached "+std::to_string(src)+" "+std::to_string(dst)+" "+std::to_string(value)+"\n");return;}
        Fw410MainMixerModel desired;const char* label=nullptr;if(c=="MAIN_MIXER_HW INIT_ORIGINAL"){desired.loadOriginalIdentityPreset();label="original-identity";}else if(c=="MAIN_MIXER_HW INIT_MACFW"){desired.loadMacfwRemappedExperimentPreset();label="macfw-remapped";}else{reply("ERR unknown-command\n");return;}std::size_t writes=0,failedSrc=0,failedDst=0;if(!writeFullMainMixer(desired,writes,failedSrc,failedDst)){mainMixerHardwareInitialized_=false;reply("ERR fcp-write-failed after "+std::to_string(writes)+" writes src="+std::to_string(failedSrc)+" dst="+std::to_string(failedDst)+"\n");return;}mainMixerModel_=desired;mainMixerHardwareInitialized_=true;reply("OK hardware-write "+std::string(label)+" 35\n");
    }
    void handleOutputPair(const std::string&c){
        const std::string gp="OUTPUT_PAIR GET ";if(c.rfind(gp,0)==0){std::istringstream in(c.substr(gp.size()));unsigned i=0;std::string x;if(!(in>>i)||(in>>x)||i>=5){reply("ERR invalid-output-pair\n");return;}std::uint8_t s=0xff;std::int16_t l=0,r=0;if(!fcp_->readSelector(kOutputSelectorBlocks[i],s)||!readStereoLevel(kOutputLevelBlocks[i],l,r)){reply("ERR fcp-read-failed\n");return;}reply("OK "+std::to_string(static_cast<unsigned>(s))+" "+std::to_string(l)+" "+std::to_string(r)+"\n");return;}
        const std::string sp="OUTPUT_PAIR SET_SOURCE ";if(c.rfind(sp,0)==0){std::istringstream in(c.substr(sp.size()));unsigned i=0,s=0;std::string x;if(!(in>>i>>s)||(in>>x)||i>=5||s>1){reply("ERR invalid-output-source\n");return;}auto fb=kOutputSelectorBlocks[i];auto v=static_cast<std::uint8_t>(s);if(!fcp_->writeSelector(fb,v)){reply("ERR fcp-write-failed\n");return;}std::uint8_t verify=0xff;if(!fcp_->readSelector(fb,verify)||verify!=v){reply("ERR verify-failed\n");return;}reply("OK "+std::to_string(i)+" "+std::to_string(static_cast<unsigned>(verify))+"\n");return;}
        const std::string lp="OUTPUT_PAIR SET_LEVEL ";if(c.rfind(lp,0)==0){std::istringstream in(c.substr(lp.size()));unsigned i=0;std::string ls,rs,x;if(!(in>>i>>ls>>rs)||(in>>x)||i>=5){reply("ERR invalid-output-level\n");return;}std::int16_t l=0,r=0;if(!parseRawLevel(ls,l)||!parseRawLevel(rs,r)){reply("ERR invalid-output-level\n");return;}if(!writeStereoLevel(kOutputLevelBlocks[i],l,r)){reply("ERR fcp-write-or-verify-failed\n");return;}reply("OK "+std::to_string(i)+" "+std::to_string(l)+" "+std::to_string(r)+"\n");return;}
        reply("ERR unknown-command\n");
    }
    void handle(const std::string&c){
        if(c=="HEADPHONE_SOURCE GET"){std::uint8_t v=0xff;if(!fcp_->readSelector(kHeadphoneSelector,v)){reply("ERR fcp-read-failed\n");return;}reply("OK "+std::to_string(static_cast<unsigned>(v))+"\n");return;}
        if(c=="HEADPHONE_SOURCE SET 0"||c=="HEADPHONE_SOURCE SET 1"){std::uint8_t v=c.back()=='1'?1:0;if(!fcp_->writeSelector(kHeadphoneSelector,v)){reply("ERR fcp-write-failed\n");return;}std::uint8_t q=0xff;if(!fcp_->readSelector(kHeadphoneSelector,q)||q!=v){reply("ERR verify-failed\n");return;}reply("OK "+std::to_string(static_cast<unsigned>(q))+"\n");return;}
        if(c=="SPDIF_CONNECTOR GET"){std::uint8_t v=0xff;if(!fcp_->readSelector(kSpdifConnectorSelector,v)){reply("ERR fcp-read-failed\n");return;}reply("OK "+std::to_string(static_cast<unsigned>(v))+"\n");return;}
        if(c.rfind("MAIN_MIXER ",0)==0){handleMainMixer(c);return;}
        if(c.rfind("MAIN_MIXER_HW ",0)==0){handleMainMixerHardware(c);return;}
        if(c.rfind("MAIN_MIXER_MODEL ",0)==0){handleMainMixerModel(c);return;}
        if(c.rfind("MAIN_STRIP_LEVEL ",0)==0){handleMainStripLevel(c);return;}
        if(c.rfind("OUTPUT_PAIR ",0)==0){handleOutputPair(c);return;}
        if(c.rfind("HEADPHONE_VOLUME ",0)==0){handleLevel(c,"HEADPHONE_VOLUME",kHeadphoneLevel);return;}
        if(c.rfind("AUX_STREAM12_VOLUME ",0)==0){handleLevel(c,"AUX_STREAM12_VOLUME",kAuxStream12Level);return;}
        if(c.rfind("AUX_OUTPUT_VOLUME ",0)==0){handleLevel(c,"AUX_OUTPUT_VOLUME",kAuxOutputLevel);return;}
        if(c.rfind("HEADPHONE_MIXER ",0)==0){handleHeadphoneMixer(c);return;}
        reply("ERR unknown-command\n");
    }
    Fw410FcpControl*fcp_=nullptr;int listenFd_=-1;int clientFd_=-1;std::string request_;Fw410MainMixerModel mainMixerModel_{};bool mainMixerHardwareInitialized_=false;
};

} // namespace macfw::transport::duplex {
