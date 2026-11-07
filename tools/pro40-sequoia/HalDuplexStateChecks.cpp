// SPDX-License-Identifier: Apache-2.0
#include "DuplexAudioFormat.hpp"
#include "DuplexAdmissionFixture.hpp"
#include "HalDuplexState.hpp"
#include <cstdio>
using namespace Pro40Audio;
namespace F=DuplexFixture;
using State=HalDuplexState<F::Session>;
static auto prepared(const NativeDuplexDescriptor &d,F::Watch &w,bool activate=true) {
    auto s=std::make_unique<F::Session>(d,73,w);F::prepare(*s);
    if(activate)assert(s->activate(d.preparedHost));return s;
}
static void sharedClients() {
    State state(125,3);F::Watch watch;const auto d=F::descriptor();const auto now=d.preparedHost;
    assert(state.add(0) && state.add(5) && state.add(99) && !state.add(5));
    auto session=prepared(d,watch,false);auto *owner=session.get();
    assert(!state.start(0,session,now) && session);assert(session->activate(now));
    assert(state.start(0,session,now) && !session);
    SourceFrameClock::Stamp stamp;assert(state.timestamp(5,now,stamp) && stamp.seed==1);
    assert(!state.timestamp(100,now,stamp));assert(state.retainIO(5,now));
    assert(state.remove(0));assert(!state.timestamp(0,now,stamp));
    const auto first=state.stop(99);assert(first.accepted && !first.runningChanged && first.drained && watch.joins==0);
    // A registered client that never individually started can read and stop.
    auto c=F::cycle(3712,2560);std::array<float,137*8> input{};
    assert(state.readInput(99,now,c,137,input));
    for(size_t f=0;f<137;++f)for(size_t ch=0;ch<8;++ch)assert(input[f*8+ch]==float(F::sample(3712+f,ch))/8388608.f);
    std::array<float,137*duplexOutputChannels> out{};for(size_t n=0;n<out.size();++n)out[n]=float(int(n)-400)/8388608.f;
    assert(state.writeOutput(99,now,c,137,out));
    std::array<int32_t,8*duplexOutputChannels> consume{};
    for(uint64_t frame=0;frame<648;frame+=8) {
        assert(owner->readPlayback(frame,consume));
        for(size_t f=0;f<8;++f)for(size_t ch=0;ch<duplexOutputChannels;++ch) {
            const auto expected=frame+f<512?0:int32_t((frame+f-512)*duplexOutputChannels+ch)-400;
            assert(consume[f*duplexOutputChannels+ch]==expected);
        }
        assert(owner->commitPlayback(frame,8));
    }
    const auto stopped=state.stop(5);assert(stopped.accepted && stopped.runningChanged && stopped.drained && stopped.cleanupOK);
    assert(watch.joins==1 && watch.destroyed==1 && !state.timestamp(99,now,stamp));
    input.fill(1);assert(!state.readInput(99,now,c,137,input));for(auto v:input)assert(v==0);
    assert(!state.stop(99).accepted);
    auto stale=prepared(d,watch);assert(!state.start(99,stale,now));stale->stopAndJoin();stale.reset();
    auto fresh=prepared(F::descriptor(20,2),watch);assert(state.start(99,fresh,now));
    assert(state.timestamp(5,now,stamp) && stamp.seed==2);assert(state.stop(5).drained);
}
static void lifetimeAndCleanup() {
    State state(125,3);F::Watch w;auto d=F::descriptor();const auto now=d.preparedHost;
    assert(state.add(1));auto session=prepared(d,w);assert(state.start(1,session,now));
    w.holdRead=true;
    std::thread callback([&]{std::array<float,8> out{};state.readInput(1,now,F::cycle(0,0),1,out);});
    while(!w.enteredRead.load())std::this_thread::yield();
    const auto stop=state.stop(1);assert(stop.accepted && stop.runningChanged && !stop.drained && stop.cleanupOK);
    assert(!state.drainStopped() && w.joins==0 && w.destroyed==0);
    auto next=prepared(F::descriptor(20,2),w);assert(!state.start(1,next,now));
    w.releaseRead=true;callback.join();assert(state.drainStopped() && w.joins==1);
    assert(state.start(1,next,now));w.cleanupOK=false;
    const auto bad=state.stop(1);assert(bad.accepted && !bad.cleanupOK && state.cleanupBlocked());
    assert(!state.drainStopped());
    auto blocked=prepared(F::descriptor(21,3),w);assert(!state.start(1,blocked,now));blocked->stopAndJoin();
}
static void faultAndExpiry() {
    for(unsigned fault=0;fault<7;++fault) {
        State state(125,3);F::Watch w;auto d=F::descriptor();const auto now=d.preparedHost;
        assert(state.add(1));auto s=prepared(d,w);auto *owner=s.get();assert(state.start(1,s,now));
        auto bytes=F::clock(d,73,9,4608,d.clock.origin+4608*500+2000);
        auto late=d.clock.origin+4608*500+2000;
        switch(fault) {
        case 0:bytes[16]^=1;break; // Wrong nonce.
        case 1:bytes[24]^=1;break; // Wrong epoch.
        case 2:bytes[40]^=1;break; // New bus generation cannot reuse the session.
        case 3:bytes[88]=8;break; // Duplicate sequence.
        case 4:late+=F::policy().maxArrivalAge+1;break;
        case 5:bytes[64]=0;bytes[65]=0;break; // Invalid rate.
        case 6:late=now+F::policy().maxUseAge+1;break;
        }
        assert(!owner->receiveClock(bytes,late));SourceFrameClock::Stamp stamp;
        assert(!state.timestamp(1,late,stamp));assert(!state.retainIO(1,late));
        HalSilenceModel::Snapshot snap;assert(state.snapshot(snap) && snap.ioHolds==1);
        assert(state.stop(1).drained); // Source loss never eats the host's hold.
    }
    F::Watch w;auto d=F::descriptor();auto late=prepared(d,w);
    State state(125,3);assert(state.add(1));assert(!state.start(1,late,d.expiresHost+1));late->stopAndJoin();
    // Fresh clock messages cannot refresh admission or an old published prefix
    // when their input samples never arrive.
    F::Watch missingWatch;auto missing=prepared(d,missingWatch);
    const auto nextNow=d.clock.origin+4608*500+2000;
    assert(missing->receiveClock(F::clock(d,73,9,4608,nextNow),nextNow));
    assert(!missing->pump(d.evidenceHost+F::policy().maxUseAge+1));missing->stopAndJoin();
    F::Watch lossWatch;auto lost=prepared(d,lossWatch);lost->requestStop();
    SourceFrameClock::Stamp stamp;assert(!lost->timestamp(d.preparedHost,stamp));lost->stopAndJoin();
    F::Watch formatWatch;auto wrong=d;wrong.inputChannels=1;
    auto format=std::make_unique<F::Session>(wrong,73,formatWatch);
    assert(format->failed() && !format->prepared(d.preparedHost));format->stopAndJoin();
    F::Watch oldWatch;auto old=d;old.outputChannels=6;
    auto oldFormat=std::make_unique<F::Session>(old,73,oldWatch);
    assert(old.validAt(d.preparedHost) && oldFormat->failed() && !oldFormat->prepared(d.preparedHost));
    oldFormat->stopAndJoin(); // A previous six-output peer cannot enter twelve-channel IO.
}
static void renewalAndReaders() {
    State state(125,3);F::Watch w;auto d=F::descriptor();assert(state.add(1) && state.add(2));
    auto s=prepared(d,w);auto *owner=s.get();assert(state.start(1,s,d.preparedHost));
    std::atomic<uint64_t> now{d.preparedHost},first{d.prefixFrames-512},reads{0};
    std::atomic<bool> done{false};std::array<std::thread,4> readers;
    for(auto &t:readers)t=std::thread([&]{
        while(!done.load()) {
            const auto frame=first.load();const auto time=now.load();std::array<float,137*8> out;
            if(state.readInput(2,time,F::cycle(frame,0),137,out)) {
                for(size_t f=0;f<137;++f)for(size_t ch=0;ch<8;++ch)
                    assert(out[f*8+ch]==float(F::sample(frame+f,ch))/8388608.f);
                ++reads;
            } else for(auto v:out)assert(v==0);
        }
    });
    SourceFrameClock::Stamp previous{};uint64_t frames=d.prefixFrames,sequence=d.evidenceSequence,stamps=0;
    for(unsigned n=0;n<9600;++n) { // 102.4 simulated seconds, no finite clock ceiling.
        const auto time=d.clock.origin+(frames+512)*500+2000;
        // Clock prefix deliberately arrives before its PCM batch.
        assert(owner->receiveClock(F::clock(d,73,++sequence,frames+512,time),time));
        assert(owner->receiveInput(F::pcm<8>(19,frames),time));frames+=512;
        // A software producer can outrun its concurrent readers arbitrarily.
        // Hold virtual time while retrying publication; callbacks never retry.
        unsigned attempts=0;
        while(owner->publishedClockSequence()<sequence) {
            assert(++attempts<1000000 && owner->pump(time));std::this_thread::yield();
        }
        now.store(time);first.store(frames-512);
        SourceFrameClock::Stamp stamp=previous;
        if(!state.timestamp(2,time,stamp)) {assert(stamp.sample==previous.sample && stamp.host==previous.host);continue;}
        assert(stamp.sample>=previous.sample && stamp.host>=previous.host);++stamps;
        if(stamp.sample==previous.sample && previous.host)assert(stamp.host==previous.host);
        previous=stamp;
    }
    done=true;for(auto &t:readers)t.join();assert(reads>0 && stamps>100);
    assert(state.timestamp(2,now.load(),previous) && previous.sample>48000*100);
    auto stop=state.stop(2);assert(stop.drained && stop.cleanupOK && w.joins==1);
}
static void delayedInputAndMissingFrames() {
    F::Watch w;auto d=F::descriptor();auto session=prepared(d,w);
    uint64_t frames=d.prefixFrames,sequence=d.evidenceSequence,now=d.preparedHost;
    while(frames<12288){
        now=d.clock.origin+(frames+512)*500+2000;
        assert(session->receiveClock(F::clock(d,73,++sequence,frames+512,now),now));
        assert(session->receiveInput(F::pcm<8>(19,frames),now));frames+=512;
    }
    // A callback delayed beyond the former 4,096-frame retention still reads
    // its original samples. It must neither skip ahead nor get padded silence.
    std::array<float,137*8> samples;const auto first=frames-5000;
    assert(session->readInput(now,F::cycle(first,0),137,samples));
    for(unsigned f=0;f<137;++f)for(unsigned c=0;c<8;++c)
        assert(samples[f*8+c]==float(F::sample(first+f,c))/8388608.f);
    for(const auto missing:{frames-8193,frames-136}) {
        samples.fill(99);assert(!session->readInput(now,F::cycle(missing,0),137,samples));
        for(auto v:samples)assert(v==0);
        const auto diagnostic=session->ownerDiagnostic();
        assert(diagnostic.inputFailure==10+unsigned(missing<frames-8192?WindowResult::stale:WindowResult::notYetAvailable));
        assert(diagnostic.inputOldest==frames-8192 && diagnostic.inputNext==frames);
    }
    assert(session->stopAndJoin());
}
int main(){sharedClients();lifetimeAndCleanup();faultAndExpiry();renewalAndReaders();delayedInputAndMissingFrames();std::puts("PASS: admitted 8/12 buffers, device-wide clients, input/output samples, pinned stop, cleanup blocking, restart/expiry/source loss, 102s renewal, four readers and delayed-input/missing-frame boundaries");}
