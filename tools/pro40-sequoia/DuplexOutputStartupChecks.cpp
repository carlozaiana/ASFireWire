// SPDX-License-Identifier: Apache-2.0
#include "DuplexAudioFormat.hpp"
#include "DuplexAdmissionFixture.hpp"
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>
using namespace Pro40Audio;
namespace F=DuplexFixture;
struct Harness {
    NativeDuplexDescriptor d=F::descriptor();
    SustainedClockPublication<8192,8> publication{d.clock.identity,F::policy()};
    PlaybackPcmQueue<8192,duplexOutputChannels> output{19};
    DuplexCallbackSession<8192,8,duplexOutputChannels> callback{publication,output,d.inputTimeline,d.outputTimeline};
    Harness() {
        auto window=std::make_unique<AudioInputWindow<8192,8>>(19,0);
        std::array<int32_t,8*8> pcm{};
        for(unsigned f=0;f<4096;f+=8)assert(window->append(19,f,pcm)==WindowResult::ok);
        assert((publication.offerProposal(d.clock,d.evidenceSequence,d.prefixFrames,d.evidenceHost,
            *window,d.preparedHost)==SustainedClockPublication<8192,8>::Result::published));
        std::array<int32_t,1024*duplexOutputChannels> prime{};
        assert(output.write(19,0,prime)==PlaybackQueueResult::ok);
    }
    bool write(uint64_t first,std::span<const float> samples) {
        auto cycle=F::cycle(0,0);
        cycle.mOutputTime.mSampleTime=double(d.outputTimeline.callbackSample)+double(first);
        return callback.writeOutput(d.preparedHost,cycle,UInt32(samples.size()/duplexOutputChannels),samples);
    }
};
static float sample(unsigned f,unsigned c) {return float(int(f%17)-8)*.0625f+float(c)*.00390625f;}
static void arbitraryFirstInterval() {
    unsigned cases=0;
    for(unsigned frames:{64u,137u,512u,4096u})for(unsigned gap:{0u,1u,120u,511u,2048u,4095u}) {
        if(1024+gap+frames>8192)continue;
        auto h=std::make_unique<Harness>();const auto first=1024+gap;
        std::vector<float> samples(frames*duplexOutputChannels);
        for(unsigned f=0;f<frames;++f)for(unsigned c=0;c<duplexOutputChannels;++c)samples[f*duplexOutputChannels+c]=sample(f,c);
        assert(h->write(first,samples));
        assert(h->output.written()==first+frames && h->output.consumed()==0);
        std::vector<int32_t> received((first+frames)*duplexOutputChannels);
        assert(h->output.read(0,received));
        for(unsigned f=0;f<first+frames;++f)for(unsigned c=0;c<duplexOutputChannels;++c)
            assert(received[f*duplexOutputChannels+c]==(f<first?0:int32_t(samples[(f-first)*duplexOutputChannels+c]*8388608.f)));
        assert(h->output.commit(0,first+frames));
        // Subsequent application samples stay contiguous; no change to mapping.
        assert(h->write(first+frames,samples));
        const auto end=first+2*frames;
        assert(!h->write(end+1,samples) && h->output.written()==end);
        assert(h->output.fault()==PlaybackQueueResult::discontinuity);
        ++cases;
    }
    std::printf("PASS: %u first-output phases/block sizes preserve exact physical frame IDs; later gaps rejected\n",cases);
}
static void rejectedFirstInterval() {
    for(unsigned fault=0;fault<7;++fault) {
        auto h=std::make_unique<Harness>();std::array<float,512*duplexOutputChannels> samples{};samples.fill(.25f);
        uint64_t first=1144;
        if(fault==0)first=1023; // Would overwrite admitted prime.
        if(fault==1)first=8192; // Gap plus callback is not writable.
        if(fault==2)first=UINT64_MAX-511; // Timeline representability/overflow.
        if(fault==3)samples.back()=NAN;
        if(fault==4)samples.back()=INFINITY;
        if(fault==5)h->publication.close();
        if(fault==6)first=8192-512+1; // Capacity with prime, one frame too many.
        assert(!h->write(first,samples));
        assert(h->output.written()==1024 && h->output.consumed()==0);
        assert(h->output.fault()!=PlaybackQueueResult::ok);
        assert(!h->write(1024,samples)); // Rejection is terminal, never a retry.
    }
    // Exact full-buffer boundary succeeds. Also permit an empty gap.
    auto h=std::make_unique<Harness>();std::array<float,512*duplexOutputChannels> samples{};
    assert(h->write(8192-512,samples) && h->output.written()==8192);
}
static void concurrentPublication() {
    // The consumer sees either the admitted prime alone or the COMPLETE gap
    // and callback. Ring wrap occurs after partially consuming the prime.
    for(unsigned attempt=0;attempt<100;++attempt) {
        auto h=std::make_unique<Harness>();std::array<int32_t,512*duplexOutputChannels> before{};
        assert(h->output.read(0,before) && h->output.commit(0,512));
        std::array<float,512*duplexOutputChannels> samples{};samples.fill(.25f);
        std::thread producer([&]{assert(h->write(8192,samples));});
        uint64_t observed;
        do {observed=h->output.written();assert(observed==1024 || observed==8704);}while(observed==1024);
        std::vector<int32_t> received(8192*duplexOutputChannels);
        assert(h->output.read(512,received));
        for(unsigned f=0;f<8192;++f)for(unsigned c=0;c<duplexOutputChannels;++c)
            assert(received[f*duplexOutputChannels+c]==(512+f<8192?0:2097152));
        assert(h->output.commit(512,8192));producer.join();
    }
    std::puts("PASS: atomic startup publication under concurrent consumption and wrap; invalid/full/stale starts leave prime unchanged");
}
int main(){arbitraryFirstInterval();rejectedFirstInterval();concurrentPublication();}
