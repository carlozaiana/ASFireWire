// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DuplexSessionBuffers.hpp"
#include <cassert>
#include <thread>
namespace Pro40Audio::DuplexFixture {
inline SustainedClockPolicy policy() {return {1024,128,8,48000,1800000,2400000,4800,8400000,512*3072,500,100};}
inline NativeDuplexDescriptor descriptor(uint64_t epoch=19,uint64_t seed=1) {
    NativeDuplexDescriptor d;
    d.clock={{epoch,seed,7,125,3},1000000,500,1,4096+48000};
    d.evidenceSequence=8;d.prefixFrames=4096;d.evidenceHost=3050000;
    d.preparedHost=d.evidenceHost;d.expiresHost=d.preparedHost+1000000;
    d.inputFirst=0;d.initialInputFrames=512;d.outputFirst=0;d.primedOutputFrames=512;
    d.inputTimeline={epoch,seed,0,0};d.outputTimeline={epoch,seed,0,2048};
    assert(d.validAt(d.preparedHost));return d;
}
inline int32_t sample(uint64_t f,size_t c) {return int32_t((f*17+c*1009)%100000)-50000;}
template<size_t Channels> auto pcm(uint64_t epoch,uint64_t first) {
    AudioFeedBatchFor<Channels> b;b.epoch=epoch;b.firstFrame=first;b.frames=512;
    for(size_t f=0;f<b.frames;++f)for(size_t c=0;c<Channels;++c)b.samples[f*Channels+c]=sample(first+f,c);
    std::array<uint8_t,AudioFeedPacketFor<Channels>::maxBytes> bytes{};size_t used=0;
    assert(AudioFeedPacketFor<Channels>::encode(b,bytes,used)==AudioFeedError::ok && used==bytes.size());return bytes;
}
inline auto clock(const NativeDuplexDescriptor &d,uint64_t nonce,uint64_t sequence,uint64_t frames,uint64_t now) {
    auto c=d.clock;c.horizonFrames=frames+policy().lookAheadFrames;
    std::array<uint8_t,NativeDuplexClockPacket::size> bytes;
    assert(NativeDuplexClockPacket::encode({nonce,c,sequence,frames,now},bytes));return bytes;
}
struct Watch {
    std::atomic<unsigned> joins{0},destroyed{0};
    std::atomic<bool> holdRead{false},enteredRead{false},releaseRead{false};
    bool cleanupOK=true;
};
// No hardware or connection thread in this fixture. Real Session must join its
// connection/native users before buffers close. Watch tests gate pin lifetime.
struct Session:DuplexSessionBuffers<> {
    Watch &watch;bool joined=false;
    Session(const NativeDuplexDescriptor &d,uint64_t nonce,Watch &w):DuplexSessionBuffers(d,nonce,policy()),watch(w){}
    ~Session(){assert(joined);++watch.destroyed;}
    bool stopAndJoin(){if(!joined){requestStop();close();joined=true;++watch.joins;}return watch.cleanupOK;}
    bool readInput(uint64_t now,const AudioServerPlugInIOCycleInfo &c,UInt32 n,std::span<float> out) const noexcept {
        if(watch.holdRead.load()) {
            watch.enteredRead.store(true);
            while(!watch.releaseRead.load())std::this_thread::yield();
        }
        return DuplexSessionBuffers::readInput(now,c,n,out);
    }
};
inline void prepare(Session &s) {
    const auto &d=s.descriptor;
    for(uint64_t f=d.inputFirst;f<d.prefixFrames;f+=512)
        assert(s.receiveInput(pcm<8>(d.clock.identity.epoch,f),d.preparedHost));
    assert(s.prepared(d.preparedHost));
}
inline AudioServerPlugInIOCycleInfo cycle(uint64_t input,uint64_t output) {
    AudioServerPlugInIOCycleInfo c{};c.mNominalIOBufferFrameSize=512;
    c.mInputTime.mSampleTime=double(input);c.mInputTime.mFlags=kAudioTimeStampSampleTimeValid;
    c.mOutputTime.mSampleTime=double(output);c.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;
    return c;
}
}
