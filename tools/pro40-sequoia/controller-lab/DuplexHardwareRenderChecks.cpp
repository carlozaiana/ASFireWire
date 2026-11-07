// SPDX-License-Identifier: Apache-2.0
#include "DuplexHardwareRender.hpp"
#include "../NativeDuplexPacket.hpp"
#include "../NativeDuplexPolicy.hpp"
#include <cassert>
#include <cstdio>
struct Host {
    Pro40Audio::NativeDuplexDescriptor descriptor;
    unsigned reads=0,writes=0,badInput=UINT32_MAX,badOutput=UINT32_MAX;
    std::array<uint64_t,6> audible{};
    Host() {
        descriptor.clock={{1,1,7,1,1},1000000000,1000000000,48000,2000000};
        descriptor.inputFirst=1234;descriptor.outputFirst=0;descriptor.primedOutputFrames=4096;
        descriptor.outputTimeline={1,1,0,16000};
    }
    bool readInput(uint64_t,const AudioServerPlugInIOCycleInfo &c,unsigned frames,std::span<float> data) {
        assert(frames==137 && c.mInputTime.mSampleTime==1234.+reads*137);
        if(reads==badInput)return false;
        ++reads;for(size_t f=0;f<data.size()/8;++f)for(unsigned ch=0;ch<8;++ch)data[f*8+ch]=float(ch+1)/16;
        return true;
    }
    bool writeOutput(uint64_t,const AudioServerPlugInIOCycleInfo &c,unsigned frames,std::span<const float> data) {
        assert(frames==137 && c.mOutputTime.mSampleTime==20096.+writes*137);
        if(writes==badOutput)return false;
        for(unsigned f=0;f<137;++f) {
            const uint64_t index=uint64_t(writes)*137+f;
            unsigned active=0;
            for(unsigned ch=0;ch<Pro40Audio::duplexOutputChannels;++ch) {
                const auto v=data[f*Pro40Audio::duplexOutputChannels+ch];assert(std::isfinite(v) && std::abs(v)<=.031624f);
                if(ch>=6)assert(v==0); // Existing speaker diagnostic excites only 1-6.
                if(v!=0) {
                    ++active;++audible[ch];
                    const auto phase=index%(18*48000);
                    assert(phase>=uint64_t(2+ch*2)*48000 && phase<uint64_t(3+ch*2)*48000);
                }
            }
            assert(active<=1);
        }
        ++writes;return true;
    }
    bool timestamp(uint64_t,Pro40Audio::SourceFrameClock::Stamp &){return true;}
};
int main() {
    const auto policy=Pro40Audio::nativeDuplexClockPolicy(125,3);
    assert(policy.valid() && !Pro40Audio::nativeDuplexClockPolicy(0,3).valid());
    Pro40Audio::NativeClockSnapshot observed;
    observed.packet.slope=511.99636262024967L;observed.bus.slope=.99997789991063468L;
    observed.maxPacketAgeTicks=1637201; // Actual rejected startup, 66.618 ms.
    auto former=policy;former.maxPacketAgeTicks=512*3072;
    assert(!former.acceptsSnapshot(observed) && policy.acceptsSnapshot(observed));
    observed.maxPacketAgeTicks=1682849;assert(policy.acceptsSnapshot(observed)); // Prior 90-second pass.
    observed.maxPacketAgeTicks=Pro40Audio::NativeClockFit::maximumPacketAgeTicks;
    assert(policy.acceptsSnapshot(observed));++observed.maxPacketAgeTicks;assert(!policy.acceptsSnapshot(observed));
    observed.maxPacketAgeTicks=1637201;observed.bus.slope=1.001L;assert(!policy.acceptsSnapshot(observed));
    observed.bus.slope=1;observed.packet.slope=500;assert(!policy.acceptsSnapshot(observed));
    using Render=Pro40Lab::DuplexHardwareRender;
    for(unsigned mode=0;mode<4;++mode) {
        Host host;Render render;uint64_t now=2000000000;
        if(mode==1)host.badInput=20;if(mode==2)host.badOutput=20;
        const auto ok=render.run(host,[&]{return mode==3 && host.reads==Render::blocks-20;},[&]{return now;},[&](uint64_t target){now=target+300;return true;});
        assert(ok==(mode==0));assert(render.completed==(mode==0?Render::blocks:mode==3?Render::blocks-20:20));
        assert(render.failure==(mode==0?0:mode==1?2:mode==2?4:1));
        assert(render.maxLateTicks==300);
        for(unsigned ch=0;ch<8;++ch)assert(render.peak[ch]==double(ch+1)/16);
        if(!mode)for(auto n:host.audible)assert(n>5*47900 && n<5*48000);
    }
    Host host;uint64_t value=0;
    assert(Render::due(host.descriptor.clock,1,48000,value) && value==1000000001);
    host.descriptor.clock.ticksNumerator=1000100000; // Device differs from nominal by 100 ppm.
    assert(Render::due(host.descriptor.clock,1,48000,value) && value==1000100001);
    assert(!Render::due(host.descriptor.clock,UINT64_MAX,1,value));
    std::puts("PASS 90-second hardware renderer: 8 inputs, five six-tone sequences, rational pacing, early failure/late stop and overflow");
}
