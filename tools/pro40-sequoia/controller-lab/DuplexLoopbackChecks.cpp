// SPDX-License-Identifier: Apache-2.0
#include "DuplexLoopback.hpp"
#include "../NativeDuplexPacket.hpp"
#include <cassert>
#include <cstdio>
using namespace Pro40Lab;
namespace {
constexpr uint64_t inputFirst=10000,outputFirst=30000;
void returning(DuplexLoopbackContent &c,int64_t delay,bool invert=false,bool filtered=false,unsigned changing=0) {
    std::fill(c.captured.begin(),c.captured.end(),0.f);
    uint32_t state=7123;
    for(auto &x:c.captured){state^=state<<13;state^=state>>17;state^=state<<5;x=float(int(state%2001)-1000)*1e-8f+.001f;}
    for(unsigned m=0;m<c.markers;++m) {
        const auto first=outputFirst-inputFirst+c.markerStart(m)+delay+changing*m;
        for(size_t f=0;f<c.markerFrames;++f) {
            double sample=c.reference[f];
            if(filtered)sample=.8*sample+(f?.2*c.reference[f-1]:0);
            c.captured[first+f]+=float((invert?-.23:.23)*sample);
        }
    }
}
struct Host {
    Pro40Audio::NativeDuplexDescriptor descriptor;
    DuplexLoopbackContent expected;
    unsigned reads=0,writes=0;
    Host(){descriptor.clock={{1,1,2,1,1},1000000000,1000000000,48000,2000000};
        descriptor.inputFirst=inputFirst;descriptor.primedOutputFrames=4096;
        descriptor.outputTimeline={1,1,0,int64_t(outputFirst-4096)};
        returning(expected,5490);}
    bool readInput(uint64_t,const AudioServerPlugInIOCycleInfo &c,unsigned frames,std::span<float> out) {
        assert(frames==137 && c.mInputTime.mSampleTime==inputFirst+reads*137);
        for(unsigned f=0;f<frames;++f)for(unsigned ch=0;ch<8;++ch)out[f*8+ch]=ch==6?expected.captured[reads*137+f]:float(ch+1)/16;
        ++reads;return true;
    }
    bool writeOutput(uint64_t,const AudioServerPlugInIOCycleInfo &c,unsigned frames,std::span<const float> out) {
        assert(frames==137 && c.mOutputTime.mSampleTime==outputFirst+writes*137);
        std::array<float,137*Pro40Audio::duplexOutputChannels> wanted{};expected.tones(writes*137,wanted);
        assert(std::equal(out.begin(),out.end(),wanted.begin()));
        for(unsigned f=0;f<frames;++f)for(unsigned ch=0;ch<Pro40Audio::duplexOutputChannels;++ch)
            assert(ch==5?std::abs(out[f*Pro40Audio::duplexOutputChannels+ch])<=.031623f:out[f*Pro40Audio::duplexOutputChannels+ch]==0);
        ++writes;return true;
    }
    bool timestamp(uint64_t,Pro40Audio::SourceFrameClock::Stamp &){return true;}
};
}
int main() {
    // Independently known offset; sign, channel choice and absolute sample
    // mapping are verified through the same renderer used by the controller.
    Host host;DuplexLoopbackRender render;uint64_t now=2000000000;
    assert(render.run(host,[]{return false;},[&]{return now;},[&](uint64_t target){now=target;return true;}));
    assert(render.completed==8409 && render.firstOutputSample==outputFirst && render.firstInput==inputFirst);
    assert(render.content.captured==host.expected.captured);
    auto report=LoopbackAnalysis::analyze(render.content,inputFirst,outputFirst);
    assert(report.valid && report.minimum==5490 && report.maximum==5490);
    for(const auto &m:report.matches)assert(m.callbackReadback==25490);
    puts("PASS 24-second renderer: output6 only, input7 only retained, five electrical returns and absolute sample offset");

    DuplexLoopbackContent c;
    for(const int delay:{-123,5490,47990}) {
        returning(c,delay,true,true);report=LoopbackAnalysis::analyze(c,inputFirst,outputFirst);
        assert(report.valid && report.minimum==delay && report.maximum==delay);
        assert(report.matches[0].correlation<-.9);
    }
    returning(c,5490,false,false,8);report=LoopbackAnalysis::analyze(c,inputFirst,outputFirst);
    assert(!report.valid && report.maximum-report.minimum==32);
    // Equal duplicate paths must never be accepted as a unique calibration.
    returning(c,5490);
    for(unsigned m=0;m<c.markers;++m)for(unsigned f=0;f<c.markerFrames;++f)
        c.captured[outputFirst-inputFirst+c.markerStart(m)+5490+24000+f]+=.23f*c.reference[f];
    report=LoopbackAnalysis::analyze(c,inputFirst,outputFirst);assert(!report.valid);
    returning(c,5490);std::fill(c.captured.begin()+300000,c.captured.begin()+350000,0.f);
    report=LoopbackAnalysis::analyze(c,inputFirst,outputFirst);assert(!report.valid);
    std::fill(c.captured.begin(),c.captured.end(),0.f);
    report=LoopbackAnalysis::analyze(c,inputFirst,outputFirst);assert(!report.valid);
    returning(c,5490);
    for(auto &x:c.captured)x=(x-.001f)*.00001f+.01f; // DC must not mask a negligible AC return.
    report=LoopbackAnalysis::analyze(c,inputFirst,outputFirst);assert(!report.valid);
    returning(c,5490);
    for(auto &x:c.captured)x=std::clamp(x*200.f,-1.f,1.f);
    report=LoopbackAnalysis::analyze(c,inputFirst,outputFirst);assert(!report.valid);
    // FFT normalization agrees with direct Pearson correlation at a known lag.
    const std::array<float,7> ref{.1f,-.2f,.07f,.01f,-.12f,.15f,.03f};
    std::array<float,90> signal{};for(unsigned i=0;i<7;++i)signal[37+i]=ref[i]*.17f+.004f;
    const auto match=LoopbackAnalysis::find(signal,ref,0,80);
    assert(match.recordedIndex==37 && std::abs(match.correlation-1)<1e-10);
    assert(!LoopbackAnalysis::find(signal,ref,85,86).valid);
    assert(!LoopbackAnalysis::find(signal,ref,0,91).valid);
    assert(!LoopbackAnalysis::find(signal,ref,0,0).valid);
    signal[2]=NAN;assert(!LoopbackAnalysis::find(signal,ref,0,80).valid);
    puts("PASS correlation: negative/positive offset, polarity, DC/noise/filter, delay changes, duplicate, missing, silent, clipped, short/nonfinite data");
}
