#include "../hal/include/macfw_fw1814_impulse_trace.h"
#include <cassert>
#include <iostream>
#include <vector>
using namespace macfw::fw1814::diagnostic;

static bool scan(Detector& detector, std::uint32_t id, bool invert=false, bool filtered=false) {
    bool matched=false;
    for (unsigned i=0;i<kTagFrames;++i) {
        auto sample=waveform(id,i);
        if (filtered) sample=.5f*sample + (i?.25f*waveform(id,i-1):0) + .25f*waveform(id,i+1);
        Point point{}; point.tick=1000+i; point.frame=i;
        matched |= detector.feed(invert?-sample:sample,point);
    }
    return matched;
}
int main() {
    Detector detector;
    detector.reset(123456);
    assert(scan(detector,123456));
    assert(detector.onset().tick==1000);
    detector.reset(123456);
    assert(!scan(detector,654321)); // a loud old impulse cannot acquire a new ID
    detector.reset(123456);
    assert(scan(detector,123456,true,true)); // analog polarity/filter tolerance
    Shared shared{};
    shared.engineCookie.store(42); shared.requestCookie.store(42);
    shared.requestId.store(123456); shared.expiry.store(UINT64_MAX);
    Endpoint endpoint; endpoint.attach(&shared,PcmRead);
    std::vector<float> interleaved(kTagFrames*4);
    for (unsigned i=0;i<kTagFrames;++i) interleaved[i*4]=waveform(123456,i);
    Point point{}; point.tick=1234; point.frame=777; point.queue=64;
    // A tag straddling arbitrarily sized reads still has its original onset.
    endpoint.block(interleaved.data(),193,4,point);
    assert(shared.points[PcmRead].id.load()==0);
    point.frame+=193; point.tick+=100;
    endpoint.block(interleaved.data()+193*4,kTagFrames-193,4,point);
    assert(shared.points[PcmRead].id.load()==123456);
    assert(shared.points[PcmRead].frame.load()==777);
    assert(shared.points[PcmRead].tick.load()==1234);
    shared.requestId.store(654321);
    endpoint.block(interleaved.data(),kTagFrames,4,point);
    assert(shared.points[PcmRead].id.load()!=654321);
    shared.requestCookie.store(99);
    assert(!endpoint.armed());
    shared.requestCookie.store(42); shared.expiry.store(0);
    assert(!endpoint.armed());
    std::cout<<"impulse trace ID, stale rejection, chunk boundary, polarity and lease tests: PASS\n";
}
