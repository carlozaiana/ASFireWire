// SPDX-License-Identifier: Apache-2.0
#include "PlaybackPcmQueue.hpp"
#include "AudioCoreAudioWrite.hpp"
#include <cassert>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
using namespace Pro40Audio;
static int32_t pattern(uint64_t f,size_t c){return int32_t((f*17+c*100001)%16000001)-8000000;}
template<class Queue> static void fill(Queue &q,uint64_t first,size_t frames) {
    std::vector<int32_t> samples(frames*Queue::channels);
    for(size_t f=0;f<frames;++f)for(size_t c=0;c<Queue::channels;++c)samples[f*Queue::channels+c]=pattern(first+f,c);
    assert(q.write(19,first,samples)==PlaybackQueueResult::ok);
}
template<size_t Channels> static void wraps() {
    PlaybackPcmQueue<521,Channels> q(19,9007199254740992ULL);
    for(unsigned n=0;n<8000;++n) {
        const auto first=q.written();fill(q,first,137);
        // Variable writer/reader sizes and a capacity not divisible by either.
        std::array<int32_t,137*Channels> out{};
        assert(q.read(first,out));
        for(unsigned f=0;f<137;++f)for(size_t c=0;c<Channels;++c)assert(out[f*Channels+c]==pattern(first+f,c));
        assert(q.consumed()==first && q.commit(first,137));
    }
}
int main() {
    wraps<1>();wraps<6>();wraps<12>();
    PlaybackPcmQueue<16,6> q(19);fill(q,0,16);
    std::array<int32_t,8*6> out{},extra{};out.fill(123);
    assert(q.write(19,16,extra)==PlaybackQueueResult::full && q.fault()==PlaybackQueueResult::ok);
    assert(q.read(0,out) && q.read(0,out) && q.consumed()==0);
    assert(q.commit(0,8));fill(q,16,8);
    assert(q.read(8,out));for(unsigned f=0;f<8;++f)for(unsigned c=0;c<6;++c)assert(out[f*6+c]==pattern(8+f,c));
    assert(q.commit(8,8) && q.read(16,out) && q.commit(16,8));
    out.fill(123);assert(!q.read(24,out) && q.fault()==PlaybackQueueResult::underrun);
    for(auto v:out)assert(v==123);
    assert(q.underrunAfterJoin().first==24 && q.underrunAfterJoin().available==0 && q.underrunAfterJoin().requested==8);
    assert(q.write(19,24,extra)==PlaybackQueueResult::underrun);
    for(unsigned error=0;error<5;++error) {
        PlaybackPcmQueue<512,6> bad(19);fill(bad,0,8);
        if(error==0)assert(bad.write(20,8,extra)==PlaybackQueueResult::identity);
        if(error==1)assert(bad.write(19,9,extra)==PlaybackQueueResult::discontinuity);
        if(error==2){extra.back()=8388608;assert(bad.write(19,8,extra)==PlaybackQueueResult::sample);extra.back()=0;}
        if(error==3)assert(!bad.commit(0,8));
        if(error==4)assert(!bad.read(1,out));
        assert(bad.written()==8 && bad.consumed()==0);
    }
    // Actual mOutputTime and actualFrames, not nominal buffer size/input time.
    PlaybackPcmQueue<512,6> callback(19,1000);
    AudioCallbackTimeline timeline{19,7,1000,2000};
    AudioServerPlugInIOCycleInfo cycle{};cycle.mNominalIOBufferFrameSize=512;
    cycle.mInputTime.mSampleTime=-50000;cycle.mInputTime.mFlags=kAudioTimeStampSampleTimeValid;
    cycle.mOutputTime.mSampleTime=2000;cycle.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;
    std::array<float,137*6> floats{};
    floats[0]=-1;floats[1]=1;floats[2]=std::nextafter(1.f,0.f);floats[3]=2;floats[4]=-2;floats[5]=.25f;
    assert(writeCoreAudioOutput(timeline,callback,19,7,cycle,137,floats).ok());
    assert(callback.clamped()==2);
    std::array<int32_t,137*6> integers{};assert(callback.read(1000,integers));
    assert(integers[0]==-8388608 && integers[1]==8388607 && integers[2]==8388607 &&
           integers[3]==8388607 && integers[4]==-8388608 && integers[5]==2097152);
    assert(callback.commit(1000,137));
    cycle.mOutputTime.mSampleTime=2137;floats.back()=NAN;
    assert(!writeCoreAudioOutput(timeline,callback,19,7,cycle,137,floats).ok());
    assert(callback.written()==1137);
    for(unsigned error=0;error<5;++error) {
        PlaybackPcmQueue<512,6> bad(19,1000);auto c=cycle;c.mOutputTime.mSampleTime=2000;floats.fill(0);
        uint64_t seed=7;UInt32 frames=137;
        if(error==0)seed=8;
        if(error==1)c.mOutputTime.mFlags=0;
        if(error==2)c.mOutputTime.mSampleTime=2000.5;
        if(error==3)frames=136;
        if(error==4){fill(bad,1000,512);c.mOutputTime.mSampleTime=2512;}
        assert(!writeCoreAudioOutput(timeline,bad,19,seed,c,frames,floats).ok());
        assert(bad.fault()!=PlaybackQueueResult::ok);
    }
    // Actual concurrent SPSC ownership. Backpressure is handled in the test's
    // producer; the queue never spins and the native read side never blocks.
    auto concurrent=std::make_unique<PlaybackPcmQueue<8192,6>>(19);
    std::atomic<bool> finished{false};
    std::thread writer([&] {
        for(unsigned n=0;n<6000;++n) {
            while(concurrent->writable()<137)std::this_thread::yield();
            fill(*concurrent,n*137,137);
        }
        finished.store(true,std::memory_order_release);
    });
    uint64_t consumed=0;
    while(consumed<6000*137) {
        const auto available=concurrent->written()-consumed;
        if(available<8){std::this_thread::yield();continue;}
        assert(concurrent->read(consumed,out));
        for(unsigned f=0;f<8;++f)for(unsigned c=0;c<6;++c)assert(out[f*6+c]==pattern(consumed+f,c));
        assert(concurrent->commit(consumed,8));consumed+=8;
    }
    writer.join();assert(finished.load());
    std::cout<<"PASS: playback queue signed PCM, arbitrary callbacks, wrap, owned read/commit, exact output timeline, clipping/NaN, sticky faults and concurrent SPSC\n";
}
