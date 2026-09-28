#include <AudioUnit/AudioUnit.h>
#include "fw1814_impulse_trace_client.h"
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../hal/include/macfw_fw1814_hal_shm.h"
#include "../hal/include/macfw_fw1814_capture_shm.h"
static void printTransportEstimate(double rate) {
    int po = shm_open(macfw::fw1814::hal::kPlaybackShmName, O_RDONLY, 0);
    int ci = shm_open(macfw::fw1814::hal::capture::kShmName, O_RDONLY, 0);
    if (po < 0 || ci < 0) { std::cout << "transport estimate unavailable\n"; if (po >= 0) close(po); if (ci >= 0) close(ci); return; }
    auto* p = static_cast<const macfw::fw1814::hal::SharedPlaybackRing*>(mmap(nullptr, sizeof(macfw::fw1814::hal::SharedPlaybackRing), PROT_READ, MAP_SHARED, po, 0));
    auto* c = static_cast<const macfw::fw1814::hal::capture::SharedCaptureRing*>(mmap(nullptr, sizeof(macfw::fw1814::hal::capture::SharedCaptureRing), PROT_READ, MAP_SHARED, ci, 0));
    if (p == MAP_FAILED || c == MAP_FAILED || !macfw::fw1814::hal::valid(*p) || !macfw::fw1814::hal::capture::valid(*c)) { std::cout << "transport estimate unavailable\n"; }
    else { const auto pq = macfw::fw1814::hal::availableFrames(*p); const auto cq = macfw::fw1814::hal::capture::availableFrames(*c); std::cout << "transport_playback_frames=" << pq << " (" << pq * 1000.0 / rate << " ms)\n" << "transport_capture_frames=" << cq << " (" << cq * 1000.0 / rate << " ms)\n" << "transport_queue_sum_frames=" << pq + cq << " (" << (pq + cq) * 1000.0 / rate << " ms)\n"; }
    if (p != MAP_FAILED) munmap(const_cast<macfw::fw1814::hal::SharedPlaybackRing*>(p), sizeof(*p)); if (c != MAP_FAILED) munmap(const_cast<macfw::fw1814::hal::capture::SharedCaptureRing*>(c), sizeof(*c)); close(po); close(ci);
}

static bool waitForFw1814Transport(double rate, double timeoutSeconds) {
    const auto expected = static_cast<std::uint32_t>(std::llround(rate));
    const auto deadline = CFAbsoluteTimeGetCurrent() + timeoutSeconds;
    bool sawReady = false;
    std::uint64_t firstDecodedFrame = 0;
    while (CFAbsoluteTimeGetCurrent() < deadline) {
        int po = shm_open(macfw::fw1814::hal::kPlaybackShmName, O_RDONLY, 0);
        int ci = shm_open(macfw::fw1814::hal::capture::kShmName, O_RDONLY, 0);
        if (po >= 0 && ci >= 0) {
            auto* p = static_cast<const macfw::fw1814::hal::SharedPlaybackRing*>(
                mmap(nullptr, sizeof(macfw::fw1814::hal::SharedPlaybackRing),
                     PROT_READ, MAP_SHARED, po, 0));
            auto* c = static_cast<const macfw::fw1814::hal::capture::SharedCaptureRing*>(
                mmap(nullptr, sizeof(macfw::fw1814::hal::capture::SharedCaptureRing),
                     PROT_READ, MAP_SHARED, ci, 0));
            if (p != MAP_FAILED && c != MAP_FAILED &&
                macfw::fw1814::hal::valid(*p) &&
                macfw::fw1814::hal::capture::valid(*c) &&
                p->sampleRate.load(std::memory_order_acquire) == expected &&
                c->sampleRate.load(std::memory_order_acquire) == expected &&
                p->active.load(std::memory_order_acquire) != 0) {
                const auto decodedFrame =
                    c->decodedFrames.load(std::memory_order_acquire);
                if (!sawReady) {
                    sawReady = true;
                    firstDecodedFrame = decodedFrame;
                } else if (decodedFrame > firstDecodedFrame) {
                    munmap(const_cast<macfw::fw1814::hal::SharedPlaybackRing*>(p),
                           sizeof(*p));
                    munmap(const_cast<macfw::fw1814::hal::capture::SharedCaptureRing*>(c),
                           sizeof(*c));
                    close(po); close(ci);
                    return true;
                }
            } else {
                sawReady = false;
            }
            if (p != MAP_FAILED)
                munmap(const_cast<macfw::fw1814::hal::SharedPlaybackRing*>(p),
                       sizeof(*p));
            if (c != MAP_FAILED)
                munmap(const_cast<macfw::fw1814::hal::capture::SharedCaptureRing*>(c),
                       sizeof(*c));
        }
        if (po >= 0) close(po);
        if (ci >= 0) close(ci);
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, .02, false);
    }
    return false;
}

struct P {
    std::uint32_t traceId = 0;
    UInt32 outputChannel = 1;
    UInt32 outputChannels = 1;
    macfw::fw1814::diagnostic::Session traceSession;
    macfw::fw1814::diagnostic::Endpoint traceOut;
    macfw::fw1814::diagnostic::Detector traceIn;
    AudioUnit u{};
    double rate{};
    std::atomic<uint64_t> out{0}, in{0};
    std::atomic<bool> sent{false}, got{false};
    std::vector<float> b;
    uint64_t t0{}, t1{}, wall0{}, wall1{};
    UInt32 outFrames{}, inFrames{}, outOffset{}, inOffset{};
    // Preserve the original electrical sample-host-time calculation.
    uint64_t hp() { return 1000000000ull / (uint64_t)rate; }

    static OSStatus outcb(void* r, AudioUnitRenderActionFlags*, const AudioTimeStamp* t,
                          UInt32, UInt32 n, AudioBufferList* list) {
        auto* p = static_cast<P*>(r);
        auto* data = static_cast<float*>(list->mBuffers[0].mData);
        const auto base = p->out.fetch_add(n);
        std::fill_n(data, n*p->outputChannels, 0);
        if (!p->sent && base <= 48000 && 48000 < base+n) {
            data[(48000-base)*p->outputChannels+p->outputChannel-1] = .8f;
            p->outFrames = n;
            p->outOffset = 48000-base;
            p->wall0 = mach_absolute_time();
            p->t0 = t->mHostTime+(48000-base)*p->hp();
            p->sent = true;
        }
        if (p->traceId) {
            // Only diagnostic mode appends an ID tag. The leading impulse and
            // its electrical/wall timestamp convention are unchanged.
            for (UInt32 i=0; i<n; ++i) {
                const auto frame = base+i;
                if (frame >= 48000 && frame < 48000+macfw::fw1814::diagnostic::kTagFrames)
                    data[i*p->outputChannels+p->outputChannel-1] = macfw::fw1814::diagnostic::waveform(p->traceId, frame-48000);
            }
            macfw::fw1814::diagnostic::Point point{};
            point.tick = base <= 48000 && 48000 < base+n ? p->wall0 : mach_absolute_time();
            point.a = t->mFlags;
            point.b = t->mHostTime;
            point.sampleHost = t->mHostTime;
            point.frame = base;
            point.callbackFrames = n;
            p->traceOut.block(data, n, p->outputChannels, point, p->hp(), p->outputChannels);
        }
        return noErr;
    }

    static OSStatus incb(void* r, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* t,
                          UInt32, UInt32 n, AudioBufferList*) {
        auto* p = static_cast<P*>(r);
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = 1;
        list.mBuffers[0].mDataByteSize = n*4;
        list.mBuffers[0].mData = p->b.data();
        const auto status = AudioUnitRender(p->u, flags, t, 1, n, &list);
        if (status) return status;
        const auto inputBase = p->in.fetch_add(n);
        if (p->traceId) {
            if (!p->got && p->sent) {
                const auto now = mach_absolute_time();
                for (UInt32 i=0; i<n; ++i) {
                    macfw::fw1814::diagnostic::Point point{};
                    point.tick = now;
                    point.a = t->mFlags;
                    point.b = t->mHostTime;
                    point.sampleHost = t->mHostTime+i*p->hp();
                    point.frame = inputBase+i;
                    point.callbackFrames = n;
                    point.offset = i;
                    if (p->traceIn.feed(p->b[i], point)) {
                        const auto& onset = p->traceIn.onset();
                        p->wall1 = onset.tick;
                        p->t1 = onset.sampleHost;
                        p->inFrames = onset.callbackFrames;
                        p->inOffset = onset.offset;
                        macfw::fw1814::diagnostic::publish(*p->traceSession.shared(),
                            macfw::fw1814::diagnostic::ToolDelivery, p->traceId, onset);
                        p->got = true;
                        break;
                    }
                }
            }
            return noErr;
        }
        if (!p->got) {
            for (UInt32 i=0; i<n; ++i) {
                if (std::fabs(p->b[i]) > .15) {
                    p->inFrames = n;
                    p->inOffset = i;
                    p->wall1 = mach_absolute_time();
                    p->t1 = t->mHostTime+i*p->hp();
                    p->got = true;
                    break;
                }
            }
        }
        return noErr;
    }
};
int main(int argc,char**argv){double requestedRate=0;
std::uint32_t traceId=0;
UInt32 outputChannel=1;
for(int i=1;i+1<argc;i++){if(std::string(argv[i])=="--rate")requestedRate=std::stod(argv[i+1]);
if(std::string(argv[i])=="--trace-id"){auto id=std::stoull(argv[i+1]);
if(!id||id>UINT32_MAX)return 3;
traceId=static_cast<std::uint32_t>(id);
}if(std::string(argv[i])=="--output-channel"){
auto channel=std::stoul(argv[i+1]);
if(channel!=1&&channel!=2){std::cerr<<"output channel must be 1 or 2\n";return 3;}
outputChannel=static_cast<UInt32>(channel);
}}if(traceId&&requestedRate){std::cerr<<"trace probes must not request a rate transition\n";
return 3;
}AudioComponentDescription d{kAudioUnitType_Output,kAudioUnitSubType_HALOutput,kAudioUnitManufacturer_Apple,0,0};
auto c=AudioComponentFindNext(nullptr,&d);
if(!c)return 1;
P p;
p.traceId=traceId;
p.outputChannel=outputChannel;
p.outputChannels=outputChannel;
if(AudioComponentInstanceNew(c,&p.u)!=noErr)return 1;
UInt32 one=1;
if(AudioUnitSetProperty(p.u,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Input,1,&one,4)||AudioUnitSetProperty(p.u,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Output,0,&one,4))return 1;
AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
UInt32 z=0;
AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&a,0,nullptr,&z);
std::vector<AudioDeviceID> ds(z/4);
AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,0,nullptr,&z,ds.data());
AudioDeviceID dev=kAudioObjectUnknown;
const std::string wanted = (argc > 2 && std::string(argv[1]) == "--device") ? argv[2] : "1814";
for(auto id:ds){AudioObjectPropertyAddress n{kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
CFStringRef s=nullptr;
UInt32 q=8;
if(!AudioObjectGetPropertyData(id,&n,0,nullptr,&q,&s)&&s){char x[128]{};
CFStringGetCString(s,x,128,kCFStringEncodingUTF8);
CFRelease(s);
std::cout<<"device "<<id<<": "<<x<<"\n";
if(std::string(x).find(wanted)!=std::string::npos)dev=id;
}}if(dev==kAudioObjectUnknown){std::cerr<<"requested device not found\n";
return 1;
}for(auto scope:{kAudioObjectPropertyScopeInput,kAudioObjectPropertyScopeOutput}){AudioObjectPropertyAddress la{kAudioDevicePropertyLatency,scope,kAudioObjectPropertyElementMain};
UInt32 v=0,ls=sizeof(v);
auto e=AudioObjectGetPropertyData(dev,&la,0,nullptr,&ls,&v);
std::cout<<"device_latency_"<<(scope==kAudioObjectPropertyScopeInput?"input":"output")<<"="<<(e==noErr?std::to_string(v):"unavailable")<<" frames\n";
}for(auto scope:{kAudioObjectPropertyScopeInput,kAudioObjectPropertyScopeOutput}){AudioObjectPropertyAddress sa{kAudioDevicePropertyStreams,scope,kAudioObjectPropertyElementMain};
UInt32 ss=0;
if(AudioObjectGetPropertyDataSize(dev,&sa,0,nullptr,&ss)==noErr&&ss){std::vector<AudioStreamID> ids(ss/sizeof(AudioStreamID));
if(AudioObjectGetPropertyData(dev,&sa,0,nullptr,&ss,ids.data())==noErr)for(auto sid:ids){AudioObjectPropertyAddress la{kAudioStreamPropertyLatency,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
UInt32 sv=0,sz2=sizeof(sv);
auto se=AudioObjectGetPropertyData(sid,&la,0,nullptr,&sz2,&sv);
std::cout<<"stream_latency_"<<(scope==kAudioObjectPropertyScopeInput?"input":"output")<<"="<<(se==noErr?std::to_string(sv):"unavailable")<<" frames\n";
}}}if(requestedRate>0){AudioObjectPropertyAddress ra{kAudioDevicePropertyNominalSampleRate,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
Float64 currentRate=0;
UInt32 rateSize=sizeof(currentRate);
auto getStatus=AudioObjectGetPropertyData(dev,&ra,0,nullptr,&rateSize,&currentRate);
if(getStatus!=noErr){std::cerr<<"could not read current sample rate: "<<getStatus<<"\n";
return 1;
}if(std::fabs(currentRate-requestedRate)<0.5){std::cerr<<"already running at requested rate "<<currentRate<<" Hz; skipping rate request\n";
}else{auto rs=AudioObjectSetPropertyData(dev,&ra,0,nullptr,sizeof(requestedRate),&requestedRate);
std::cerr<<"rate request status="<<rs<<" ("<<currentRate<<" -> "<<requestedRate<<" Hz)\n";
if(rs!=noErr)return 1;
if(wanted=="1814"){std::cerr<<"waiting for FW1814 transport readiness...\n";
if(!waitForFw1814Transport(requestedRate,8.0)){std::cerr<<"FW1814 transport did not become ready at "<<requestedRate<<" Hz\n";
return 1;
}}else{auto until=CFAbsoluteTimeGetCurrent()+2.0;
while(CFAbsoluteTimeGetCurrent()<until)CFRunLoopRunInMode(kCFRunLoopDefaultMode,.01,false);
}}}AudioUnitSetProperty(p.u,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&dev,4);
AudioStreamBasicDescription f{};
z=sizeof(f);
if(AudioUnitGetProperty(p.u,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&f,&z))return 1;
p.rate=f.mSampleRate;
p.b.resize(4096);
if(traceId){if(wanted!="1814"||p.rate!=48000||!p.traceSession.arm(traceId)){std::cerr<<"48 kHz trace unavailable/busy: install instrumented HAL and 48 engine; no rate change attempted\n";
return 3;
}p.traceOut.attach(p.traceSession.shared(),macfw::fw1814::diagnostic::ToolSubmit);
p.traceIn.reset(traceId);
}AudioObjectPropertyAddress ba{kAudioDevicePropertyBufferFrameSize,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
UInt32 bf=0,bs=sizeof(bf);
auto be=AudioObjectGetPropertyData(dev,&ba,0,nullptr,&bs,&bf);
std::cout<<"device_buffer_frames="<<(be==noErr?std::to_string(bf):"unavailable")<<"\n";
std::cout<<"running at "<<p.rate<<" Hz\n";
AudioStreamBasicDescription cfmt{p.rate,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked|kAudioFormatFlagsNativeEndian,4,1,4,1,32,0};
AudioStreamBasicDescription ofmt=cfmt;
ofmt.mBytesPerPacket=4*p.outputChannels;
ofmt.mBytesPerFrame=4*p.outputChannels;
ofmt.mChannelsPerFrame=p.outputChannels;
if(AudioUnitSetProperty(p.u,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&ofmt,sizeof(ofmt)) ||
   AudioUnitSetProperty(p.u,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,1,&cfmt,sizeof(cfmt))){
std::cerr<<"could not set AUHAL stream formats\n";return 1;}
AURenderCallbackStruct o{P::outcb,&p},i{P::incb,&p};
AudioUnitSetProperty(p.u,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&o,sizeof(o));
AudioUnitSetProperty(p.u,kAudioOutputUnitProperty_SetInputCallback,kAudioUnitScope_Global,0,&i,sizeof(i));
UInt32 m=4096;
AudioUnitSetProperty(p.u,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&m,4);
if(AudioUnitInitialize(p.u)||AudioOutputUnitStart(p.u))return 1;
auto end=CFAbsoluteTimeGetCurrent()+6;
while(CFAbsoluteTimeGetCurrent()<end&&!p.got)CFRunLoopRunInMode(kCFRunLoopDefaultMode,.01,false);
AudioOutputUnitStop(p.u);
AudioUnitUninitialize(p.u);
AudioComponentInstanceDispose(p.u);
mach_timebase_info_data_t reportingTimebase{};
mach_timebase_info(&reportingTimebase);
std::cout<<"mach_timebase_numer="<<reportingTimebase.numer<<" mach_timebase_denom="<<reportingTimebase.denom<<"\n"<<"callback_output_frames="<<p.outFrames<<" callback_input_frames="<<p.inFrames<<" impulse_output_offset="<<p.outOffset<<" impulse_input_offset="<<p.inOffset<<"\n"<<"marker-tool-out-scheduled="<<p.t0<<" marker-tool-in-sample="<<p.t1<<"\n";
if(traceId)p.traceSession.finish();
if(!p.got){std::cout<<"marker-tool-out="<<p.wall0<<" marker-tool-in="<<p.wall1<<"\n";
std::cout<<"impulse not returned\n";
return 2;
}double ns=(p.t1-p.t0)*1e-9;
mach_timebase_info_data_t tb{};
mach_timebase_info(&tb);
double wallNs=p.wall1>p.wall0?static_cast<double>(p.wall1-p.wall0)*tb.numer/tb.denom:0.0;
std::cout<<"round_trip_seconds="<<ns<<" round_trip_frames="<<ns*p.rate<<"\n";
std::cout<<"callback_wall_seconds="<<wallNs*1e-9<<" callback_wall_frames="<<wallNs*1e-9*p.rate<<"\n";
std::cout<<"marker-tool-out="<<p.wall0<<" marker-tool-in="<<p.wall1<<"\n";
if(wanted == "1814") printTransportEstimate(p.rate);
 else std::cout<<"transport estimate skipped for non-FW1814 device\n";
}
