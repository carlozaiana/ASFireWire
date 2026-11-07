// SPDX-License-Identifier: Apache-2.0
#include "DuplexAudioFormat.hpp"
#include "HalDuplexMachSession.hpp"
#include "HalSilenceDriver.hpp"
#include "NativeDuplexSourceConnection.hpp"
#include "NativeOriginalPeerCapture.hpp"
#include "SustainedInputReceiver.hpp"
#include "DuplexGeneratedSource.hpp"
#include <cassert>
#include <cstdio>
#include <unistd.h>
using namespace Pro40Audio;
using namespace std::chrono_literals;
using Wall=std::chrono::steady_clock;
using K=DuplexMachChannel::Kind;
using R=DuplexMachChannel::Result;
static mach_port_t port() {
    mach_port_t p=MACH_PORT_NULL;assert(mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&p)==KERN_SUCCESS);
    assert(mach_port_insert_right(mach_task_self(),p,p,MACH_MSG_TYPE_MAKE_SEND)==KERN_SUCCESS);return p;
}
static audit_token_t selfToken() {
    const auto p=port();mach_msg_header_t h{};h.msgh_remote_port=p;h.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,0);h.msgh_size=sizeof(h);
    assert(mach_msg(&h,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(h),0,MACH_PORT_NULL,0,MACH_PORT_NULL)==KERN_SUCCESS);
    NativeMachReceived<> received;assert(received.read(p,100,nativeAuditReceiveOptions));
    auto capture=NativeOriginalPeerCapture::capture(received,p,getpid());assert(capture);
    assert(mach_port_deallocate(mach_task_self(),p)==KERN_SUCCESS);
    assert(mach_port_mod_refs(mach_task_self(),p,MACH_PORT_RIGHT_RECEIVE,-1)==KERN_SUCCESS);return capture->token();
}
static auto channels(audit_token_t a,audit_token_t b,uint64_t epoch=19) {
    const auto x=port(),y=port();
    return std::pair(std::make_unique<DuplexMachChannel>(x,y,a,epoch,73),std::make_unique<DuplexMachChannel>(y,x,b,epoch,73));
}
static void channelChecks() {
    const auto token=selfToken();auto [a,b]=channels(token,token);DuplexMachChannel::Packet packet;
    assert(b->poll(packet)==R::idle);unsigned sent=0;
    while(a->send(K::activate)==R::ok){assert(++sent<1000);}assert(a->healthy() && sent>0);
    for(unsigned i=0;i<sent;++i)assert(b->poll(packet)==R::ok && packet.kind==K::activate && !packet.used);
    assert(b->poll(packet)==R::idle && a->send(K::stop)==R::ok && b->poll(packet)==R::ok);
    assert(a->close() && a->close() && b->close());
    // Exact token mismatch rejects a kernel-delivered message without changing output.
    auto wrong=token;reinterpret_cast<unsigned char *>(&wrong)[0]^=1;
    auto [c,d]=channels(token,wrong);packet.kind=K::clock;packet.used=4;packet.bytes[0]=99;
    assert(c->send(K::stop)==R::ok && d->poll(packet)==R::invalid && packet.kind==K::clock && packet.bytes[0]==99);
    assert(c->close() && d->close());
    // Retained send right permits deliberately malformed/replayed envelopes.
    for(unsigned fault=0;fault<6;++fault) {
        const auto x=port(),y=port();DuplexMachChannel receiver(y,x,token,19,73);
        struct Raw {mach_msg_header_t h{};std::array<uint8_t,36> b{};} m;
        m.h.msgh_remote_port=y;m.h.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,0);m.h.msgh_id=DuplexMachChannel::messageID;
        m.h.msgh_size=sizeof(m.h)+32;
        DuplexWire::put(m.b,0,4,6);DuplexWire::put(m.b,8,8,19);DuplexWire::put(m.b,16,8,73);DuplexWire::put(m.b,24,8,1);
        if(fault==0)DuplexWire::put(m.b,8,8,20);
        if(fault==1)DuplexWire::put(m.b,16,8,74);
        if(fault==2)DuplexWire::put(m.b,24,8,2);
        if(fault==3)DuplexWire::put(m.b,4,4,4);
        if(fault==4)m.h.msgh_id++;
        auto send=[&]{assert(mach_msg(&m.h,MACH_SEND_MSG|MACH_SEND_TIMEOUT,m.h.msgh_size,0,MACH_PORT_NULL,0,MACH_PORT_NULL)==KERN_SUCCESS);};
        send();
        if(fault==5){assert(receiver.poll(packet)==R::ok);send();}
        assert(receiver.poll(packet)==R::invalid);assert(receiver.close());
        assert(mach_port_deallocate(mach_task_self(),y)==KERN_SUCCESS);
        assert(mach_port_mod_refs(mach_task_self(),x,MACH_PORT_RIGHT_RECEIVE,-1)==KERN_SUCCESS);
    }
    std::puts("PASS: real Mach audit token binding, exact envelope/session/sequence rejection, full-queue retry and port cleanup");
}
using Source=DuplexGeneratedSource;
static AudioServerPlugInIOCycleInfo cycle(uint64_t input,uint64_t output) {
    AudioServerPlugInIOCycleInfo c{};c.mNominalIOBufferFrameSize=512;
    c.mInputTime.mSampleTime=double(input);c.mInputTime.mFlags=kAudioTimeStampSampleTimeValid;
    c.mOutputTime.mSampleTime=double(output);c.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;return c;
}
static int32_t outputSample(uint64_t frame,unsigned c){return int32_t((frame-512)*duplexOutputChannels+c)-400;}
static void connectionCheck(unsigned scenario) {
    auto source=std::make_unique<Source>();auto d=source->descriptor();const auto token=selfToken();auto [a,b]=channels(token,token);
    auto output=std::make_unique<PlaybackPcmQueue<8192,duplexOutputChannels>>(19);
    auto peer=std::make_unique<NativeDuplexSourceConnection<SustainedInputReceiver<>>>(std::move(a),*source->receiver,*output,d,source->policy(),mach_absolute_time());
    assert(!peer->failed());
    std::atomic<bool> cancel{false},exit{false},consume{true};std::atomic<uint64_t> checked{0},inputNext{d.prefixFrames};
    std::thread native([&]{
        auto next=Wall::now()+16ms;bool confirmed=false;
        while(!exit.load() && !peer->closed()) {
            if(Wall::now()>=next && !peer->stopRequested() && scenario!=5) {source->step();inputNext=source->receiver->publishedNext();next+=16ms;}
            peer->poll(mach_absolute_time());
            if(peer->primed() && consume.load()) {
                std::array<int32_t,8*duplexOutputChannels> samples;
                while(output->written()-output->consumed()>=8) {
                    const auto f=output->consumed();assert(output->read(f,samples));
                    for(unsigned n=0;n<8;++n)for(unsigned c=0;c<duplexOutputChannels;++c)assert(samples[n*duplexOutputChannels+c]==(f+n<512?0:outputSample(f+n,c)));
                    assert(output->commit(f,8));checked=f+8;
                }
            }
            if(peer->stopRequested() && !confirmed && scenario!=2) {
                // This fixture has no native thread/DMA users. In production
                // this call must follow the actual native stop/join barrier.
                assert(peer->confirmStopped(scenario!=1));confirmed=true;
            }
            std::this_thread::sleep_for(100us);
        }
    });
    HalDuplexPreparation prepared;HalDuplexSession *owner=nullptr;
    AudioServerPlugInDriverRef driver=nullptr;AudioServerPlugInHostInterface tableHost{};unsigned factoryCalls=0;
    if(scenario==1 || scenario==3) {
        tableHost.PropertiesChanged=[](AudioServerPlugInHostRef,AudioObjectID,UInt32,const AudioObjectPropertyAddress *)->OSStatus{return noErr;};
        driver=static_cast<AudioServerPlugInDriverRef>(Pro40SilenceFactory(nullptr,kAudioServerPlugInTypeUUID));assert(driver);
        auto &api=**driver;assert(api.Initialize(driver,&tableHost)==noErr);AudioServerPlugInClientInfo client{};client.mClientID=11;
        assert(api.AddDeviceClient(driver,2,&client)==noErr);
        assert(Pro40DuplexConfigure(driver,[&](uint64_t seed,uint32_t numer,uint32_t denom) {
            ++factoryCalls;assert(seed==d.clock.identity.seed && numer==source->tb.numer && denom==source->tb.denom);
            auto result=HalDuplexMachSession::prepare(std::move(b),d.clock.identity,source->policy(),300ms,100ms,cancel);
            owner=result.session.get();return result;
        }));
        assert(api.StartIO(driver,2,11)==noErr && owner);
    } else {
        prepared=HalDuplexMachSession::prepare(std::move(b),d.clock.identity,source->policy(),300ms,100ms,cancel);
        assert(prepared.session && prepared.cleanupOK);owner=prepared.session.get();
    }
    auto &host=*owner;
    const auto read=[&](const auto &c,std::span<float> out) {
        if(!driver)return host.readInput(mach_absolute_time(),c,137,out);
        return (**driver).DoIOOperation(driver,2,3,11,kAudioServerPlugInIOOperationReadInput,137,&c,out.data(),nullptr)==noErr && !Pro40DuplexInputFailures(driver);
    };
    const auto write=[&](const auto &c,std::span<const float> in) {
        if(!driver)return host.writeOutput(mach_absolute_time(),c,137,in);
        return (**driver).DoIOOperation(driver,2,4,11,kAudioServerPlugInIOOperationWriteMix,137,&c,const_cast<float *>(in.data()),nullptr)==noErr;
    };
    std::array<float,137*8+2> input;input.fill(99);auto c=cycle(d.inputFirst+37,512);
    assert(read(c,std::span(input).subspan(1,137*8)));
    for(unsigned f=0;f<137;++f)for(unsigned ch=0;ch<8;++ch)assert(input[1+f*8+ch]==float(Fixture::sample(d.inputFirst+37+f,ch))/8388608.f);
    assert(input.front()==99 && input.back()==99);
    std::array<float,137*duplexOutputChannels> samples;uint64_t written=512;
    // Non-eight-aligned callbacks are rebatched without padding or duplication.
    for(unsigned n=0;n<(scenario==3?400u:scenario==5?10u:40u);++n) {
        for(unsigned f=0;f<137;++f)for(unsigned ch=0;ch<duplexOutputChannels;++ch)samples[f*duplexOutputChannels+ch]=float(outputSample(written+f,ch))/8388608.f;
        c=cycle(0,written);if(!write(c,samples)){std::fprintf(stderr,"output failure scenario=%u block=%u written=%llu consumed=%llu\n",scenario,n,(unsigned long long)written,(unsigned long long)checked.load());host.stopAndJoin();const auto diag=static_cast<HalDuplexMachSession &>(host).diagnosticAfterJoin();std::fprintf(stderr,"diagnostic failed=%u output=%u clock=%u sequence=%llu admission=%u\n",diag.failed,unsigned(diag.output),unsigned(diag.clock),(unsigned long long)diag.sequence,diag.outputAdmission);assert(false);}written+=137;
        std::this_thread::sleep_for(1ms);
    }
    const auto deadline=Wall::now()+100ms;
    while(checked.load()<written/8*8 && Wall::now()<deadline)std::this_thread::sleep_for(100us);
    assert(checked==written/8*8);SourceFrameClock::Stamp stamp;assert(host.timestamp(mach_absolute_time(),stamp));
    if(scenario==0) {
        // Stop must still pass a full remote playback queue and held local tail.
        consume=false;
        for(unsigned n=0;n<90;++n) {
            for(unsigned f=0;f<137;++f)for(unsigned ch=0;ch<duplexOutputChannels;++ch)samples[f*duplexOutputChannels+ch]=float(outputSample(written+f,ch))/8388608.f;
            c=cycle(0,written);assert(write(c,samples));written+=137;
            std::this_thread::sleep_for(100us);
        }
    }
    if(scenario==0) {
        const auto fullDeadline=Wall::now()+100ms;
        while(output->written()-output->consumed()<8192 && Wall::now()<fullDeadline)std::this_thread::sleep_for(100us);
        assert(output->written()-output->consumed()==8192);
    }
    if(scenario==3) {
        const auto first=inputNext.load()-1536;c=cycle(first,0);input.fill(99);
        assert(read(c,std::span(input).subspan(1,137*8)));
        for(unsigned f=0;f<137;++f)for(unsigned ch=0;ch<8;++ch)assert(input[1+f*8+ch]==float(Fixture::sample(first+f,ch))/8388608.f);
    }
    if(scenario==4){exit=true;native.join();peer.reset();}
    if(scenario==4 || scenario==5) {
        const auto expiry=Wall::now()+1500ms;
        while(host.timestamp(mach_absolute_time(),stamp) && Wall::now()<expiry)std::this_thread::sleep_for(1ms);
        assert(!host.timestamp(mach_absolute_time(),stamp));input.fill(99);
        assert(!host.readInput(mach_absolute_time(),c,137,std::span(input).subspan(1,137*8)));
        for(unsigned n=1;n<input.size()-1;++n)assert(input[n]==0);
    }
    const bool expectedClean=scenario==0 || scenario==3 || scenario==5;
    bool clean=false;
    if(driver) {
        auto &api=**driver;assert(Pro40DuplexInputFailures(driver)==0 && Pro40DuplexOutputFailures(driver)==0);
        Float64 sample;UInt64 time,seed;assert(api.GetZeroTimeStamp(driver,2,11,&sample,&time,&seed)==noErr && seed==1);
        assert(api.StopIO(driver,2,11)==noErr);
        if(scenario==1)assert(api.StartIO(driver,2,11)!=noErr && factoryCalls==1);
        assert(api.Release(driver)==0);clean=expectedClean;
    } else {
        clean=host.stopAndJoin();assert(clean==expectedClean && host.stopAndJoin()==clean);
        input.fill(99);assert(!host.readInput(mach_absolute_time(),c,137,std::span(input).subspan(1,137*8)));
        for(unsigned n=1;n<input.size()-1;++n)assert(input[n]==0);assert(input.front()==99 && input.back()==99);
    }
    exit=true;if(native.joinable())native.join();prepared.session.reset();
    if(peer && scenario!=2)assert(peer->cleanupOK()==expectedClean);
    if(scenario==0)assert(output->written()-output->consumed()==8192);
    std::printf("PASS: native packet parser/rolling fit -> Mach -> HAL session, 8 inputs/12 output exact PCM; stop scenario=%u clean=%u frames=%llu\n",scenario,clean,(unsigned long long)checked.load());
}
static void cancelledStartup() {
    const auto token=selfToken();auto source=std::make_unique<Source>();const auto d=source->descriptor();
    auto [a,b]=channels(token,token);std::atomic<bool> cancel{false};
    std::array<uint8_t,NativeDuplexPacket::size> offer;
    assert(NativeDuplexPacket::encode({NativeDuplexMessage::Kind::offer,73,d},offer));
    assert(a->send(K::start,offer)==R::ok);
    std::thread peer([&]{
        const auto deadline=Wall::now()+500ms;DuplexMachChannel::Packet p;bool acknowledged=false;
        while(Wall::now()<deadline) {
            const auto r=a->poll(p);assert(r==R::ok || r==R::idle);
            if(r==R::ok && p.kind==K::start)cancel=true;
            if(r==R::ok && p.kind==K::stop) {
                std::array<uint8_t,4> clean{1,0,0,0};assert(a->send(K::stopped,clean)==R::ok);acknowledged=true;break;
            }
            std::this_thread::sleep_for(100us);
        }
        assert(acknowledged && a->close());
    });
    auto result=HalDuplexMachSession::prepare(std::move(b),d.clock.identity,source->policy(),300ms,100ms,cancel);
    assert(!result.session && result.cleanupOK);peer.join();
    auto [c,e]=channels(token,token);cancel=true;
    result=HalDuplexMachSession::prepare(std::move(e),d.clock.identity,source->policy(),300ms,100ms,cancel);
    assert(!result.session && !result.cleanupOK && c->close());
    auto [f,g]=channels(token,token);
    result=HalDuplexMachSession::prepare(std::move(g),d.clock.identity,source->policy(),std::chrono::milliseconds::max(),100ms,cancel);
    assert(!result.session && !result.cleanupOK && f->close());
    std::puts("PASS: startup cancellation after echo joins/cleans; no offer cannot claim remote cleanup");
}
int main(){channelChecks();cancelledStartup();for(unsigned scenario=0;scenario<6;++scenario)connectionCheck(scenario);}
