// SPDX-License-Identifier: Apache-2.0
#include "DuplexAudioFormat.hpp"
#include "HalSilenceDriver.hpp"
#include "HalSilenceProperties.hpp"
#include "DuplexAdmissionFixture.hpp"
#include <cstdio>
using namespace Pro40Audio;
namespace F=DuplexFixture;
static AudioServerPlugInDriverRef driver=nullptr;
static unsigned notices=0;
static AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,AudioObjectPropertyScope scope=kAudioObjectPropertyScopeGlobal) {
    return {selector,scope,kAudioObjectPropertyElementMain};
}
static OSStatus notify(AudioServerPlugInHostRef,AudioObjectID object,UInt32 count,const AudioObjectPropertyAddress *a) {
    assert(count==1 && (object==2 || object==3 || object==4));UInt32 value=99,used=0;auto &api=**driver;
    assert(api.GetPropertyData(driver,object,0,a,0,nullptr,sizeof(value),&used,&value)==noErr && value<=1);
    AudioServerPlugInClientInfo temp{};temp.mClientID=99;
    assert(api.AddDeviceClient(driver,2,&temp)==noErr && api.RemoveDeviceClient(driver,2,&temp)==noErr);
    ++notices;return kAudioHardwareUnspecifiedError; // Committed changes are not rolled back.
}
struct Watch {unsigned prepared=0,joined=0;bool cleanupOK=true,activate=true;};
struct FixtureSession final:HalDuplexSession {
    mutable DuplexSessionBuffers<> buffers;
    Watch &watch;bool joined=false;
    FixtureSession(NativeDuplexDescriptor d,SustainedClockPolicy p,Watch &w):HalDuplexSession(d),buffers(d,73,p),watch(w) {
        for(uint64_t f=0;f<d.prefixFrames;f+=512)assert(buffers.receiveInput(F::pcm<8>(d.clock.identity.epoch,f),d.preparedHost));
        if(w.activate)assert(buffers.activate(d.preparedHost));
    }
    ~FixtureSession(){stopAndJoin();}
    bool ready(uint64_t now) const noexcept override {return buffers.ready(now);}
    bool ended() const noexcept override {return buffers.failed();}
    void requestStop() const noexcept override {buffers.requestStop();}
    bool stopAndJoin() override {if(!joined){buffers.close();joined=true;++watch.joined;}return watch.cleanupOK;}
    bool timestamp(uint64_t now,SourceFrameClock::Stamp &out) const noexcept override {return buffers.timestamp(now,out);}
    bool readInput(uint64_t now,const AudioServerPlugInIOCycleInfo &c,UInt32 frames,std::span<float> out) const noexcept override {return buffers.readInput(now,c,frames,out);}
    bool writeOutput(uint64_t now,const AudioServerPlugInIOCycleInfo &c,UInt32 frames,std::span<const float> out) const noexcept override {return buffers.writeOutput(now,c,frames,out);}
};
static auto make(Watch &watch,FixtureSession *&owner) {
    return [&watch,&owner](uint64_t seed,uint32_t numer,uint32_t denom)->HalDuplexPreparation {
        ++watch.prepared;const auto now=mach_absolute_time();
        const auto ticks=[&](uint64_t ns){return uint64_t((__uint128_t(ns)*denom+numer-1)/numer);};
        auto d=F::descriptor(100+seed,seed);
        d.clock.identity.numer=numer;d.clock.identity.denom=denom;
        d.clock.ticksNumerator=uint64_t(1000000000)*denom;d.clock.ticksDenominator=uint64_t(48000)*numer;
        d.clock.origin=now-uint64_t((__uint128_t(d.prefixFrames)*d.clock.ticksNumerator+d.clock.ticksDenominator-1)/d.clock.ticksDenominator)-ticks(10000000);
        d.evidenceHost=d.preparedHost=now;d.expiresHost=now+ticks(100000000);
        auto p=F::policy();p.maxArrivalAge=ticks(150000000);p.maxUseAge=ticks(200000000);
        p.maxCorrection=ticks(200000);p.maxStampLag=ticks(350000000);
        assert(d.validAt(now));auto result=std::make_unique<FixtureSession>(d,p,watch);owner=result.get();return {std::move(result),true};
    };
}
template<class T> static T property(AudioObjectID object,AudioObjectPropertySelector selector,AudioObjectPropertyScope scope=kAudioObjectPropertyScopeGlobal) {
    const auto a=address(selector,scope);T out{};UInt32 needed=0,used=0;auto &api=**driver;
    assert(api.HasProperty(driver,object,0,&a));Boolean settable=true;
    assert(api.IsPropertySettable(driver,object,0,&a,&settable)==noErr && !settable);
    assert(api.GetPropertyDataSize(driver,object,0,&a,0,nullptr,&needed)==noErr && needed==sizeof(out));
    assert(api.GetPropertyData(driver,object,0,&a,0,nullptr,sizeof(out),&used,&out)==noErr && used==sizeof(out));return out;
}
static AudioServerPlugInHostInterface host() {AudioServerPlugInHostInterface h{};h.PropertiesChanged=notify;return h;}
int main() {
    auto h=host();driver=static_cast<AudioServerPlugInDriverRef>(Pro40SilenceFactory(nullptr,kAudioServerPlugInTypeUUID));assert(driver);
    auto &api=**driver;assert(api.Initialize(driver,&h)==noErr);
    const auto owned=property<std::array<AudioObjectID,2>>(2,kAudioObjectPropertyOwnedObjects);assert(owned[0]==3 && owned[1]==4);
    assert(property<AudioObjectID>(2,kAudioDevicePropertyStreams,kAudioObjectPropertyScopeInput)==3);
    assert(property<AudioObjectID>(2,kAudioDevicePropertyStreams,kAudioObjectPropertyScopeOutput)==4);
    assert((property<std::array<AudioObjectID,2>>(2,kAudioDevicePropertyStreams)==owned));
    assert(property<UInt32>(3,kAudioStreamPropertyDirection)==1 && property<UInt32>(4,kAudioStreamPropertyDirection)==0);
    for(const auto object:{3u,4u}) {
        const auto f=property<AudioStreamBasicDescription>(object,kAudioStreamPropertyVirtualFormat);
        assert(f.mSampleRate==48000 && f.mChannelsPerFrame==(object==3?duplexInputChannels:duplexOutputChannels) &&
            f.mBitsPerChannel==32 && f.mBytesPerFrame==f.mChannelsPerFrame*4 && f.mBytesPerPacket==f.mBytesPerFrame);
        const auto physical=property<AudioStreamBasicDescription>(object,kAudioStreamPropertyPhysicalFormat);
        assert(physical.mChannelsPerFrame==f.mChannelsPerFrame);
    }
    auto uid=property<CFStringRef>(2,kAudioDevicePropertyDeviceUID);auto a=address(kAudioPlugInPropertyTranslateUIDToDevice);
    AudioObjectID found=0;UInt32 used=0;
    assert(api.GetPropertyData(driver,1,0,&a,sizeof(uid),&uid,sizeof(found),&used,&found)==noErr && found==2);CFRelease(uid);
    a=address(kAudioObjectPropertyOwnedObjects);AudioClassID wrong=kAudioBoxClassID;
    assert(api.GetPropertyData(driver,2,0,&a,sizeof(wrong),&wrong,0,&used,&found)==noErr && used==0);
    AudioServerPlugInClientInfo ca{},cb{};ca.mClientID=11;cb.mClientID=22;
    assert(api.AddDeviceClient(driver,2,&ca)==noErr && api.AddDeviceClient(driver,2,&cb)==noErr);
    Boolean will=false,inPlace=false;
    assert(api.WillDoIOOperation(driver,2,11,kAudioServerPlugInIOOperationReadInput,&will,&inPlace)==noErr && will && inPlace);
    assert(api.WillDoIOOperation(driver,2,11,kAudioServerPlugInIOOperationWriteMix,&will,&inPlace)==noErr && will && inPlace);
    assert(api.WillDoIOOperation(driver,2,11,kAudioServerPlugInIOOperationProcessOutput,&will,&inPlace)==noErr && !will);
    Watch watch;FixtureSession *owner=nullptr;assert(Pro40DuplexConfigure(driver,make(watch,owner)));
    assert(!Pro40DuplexConfigure(driver,make(watch,owner)));
    for(uint64_t seed=1;seed<=2;++seed) {
        assert(api.StartIO(driver,2,11)==noErr && api.StartIO(driver,2,22)==noErr);
        Float64 sample=99;UInt64 stamp=99,clockSeed=99;
        assert(api.GetZeroTimeStamp(driver,2,22,&sample,&stamp,&clockSeed)==noErr && clockSeed==seed);
        assert(api.RemoveDeviceClient(driver,2,&ca)==noErr && api.StopIO(driver,2,22)==noErr);
        auto c=F::cycle(37,2560);std::array<float,137*8+2> in;in.fill(99);
        assert(api.BeginIOOperation(driver,2,22,kAudioServerPlugInIOOperationReadInput,137,&c)==noErr);
        assert(api.DoIOOperation(driver,2,3,22,kAudioServerPlugInIOOperationReadInput,137,&c,in.data()+1,nullptr)==noErr);
        assert(api.EndIOOperation(driver,2,22,kAudioServerPlugInIOOperationReadInput,137,&c)==noErr);
        assert(in.front()==99 && in.back()==99);
        for(size_t f=0;f<137;++f)for(size_t ch=0;ch<8;++ch)assert(in[1+f*8+ch]==float(F::sample(37+f,ch))/8388608.f);
        std::array<float,137*duplexOutputChannels+2> out;out.fill(99);for(size_t n=0;n<137*duplexOutputChannels;++n)out[n+1]=float(int(n)-400)/8388608.f;
        const auto original=out;
        assert(api.DoIOOperation(driver,2,4,22,kAudioServerPlugInIOOperationWriteMix,137,&c,out.data()+1,nullptr)==noErr && out==original);
        std::array<int32_t,8*duplexOutputChannels> native;
        for(uint64_t frame=0;frame<648;frame+=8) {
            assert(owner->buffers.readPlayback(frame,native));
            for(size_t f=0;f<8;++f)for(size_t ch=0;ch<duplexOutputChannels;++ch)assert(native[f*duplexOutputChannels+ch]==(frame+f<512?0:int32_t((frame+f-512)*duplexOutputChannels+ch)-400));
            assert(owner->buffers.commitPlayback(frame,8));
        }
        assert(api.DoIOOperation(driver,2,3,22,kAudioServerPlugInIOOperationWriteMix,137,&c,out.data()+1,nullptr)!=noErr);
        assert(api.DoIOOperation(driver,2,4,22,kAudioServerPlugInIOOperationReadInput,137,&c,in.data()+1,nullptr)!=noErr);
        assert(api.DoIOOperation(driver,2,3,22,kAudioServerPlugInIOOperationReadInput,4097,&c,in.data()+1,nullptr)!=noErr);
        owner->requestStop();sample=99;stamp=99;clockSeed=99;
        assert(api.GetZeroTimeStamp(driver,2,22,&sample,&stamp,&clockSeed)!=noErr && sample==99 && stamp==99 && clockSeed==99);
        in.fill(99);assert(api.DoIOOperation(driver,2,3,22,kAudioServerPlugInIOOperationReadInput,137,&c,in.data()+1,nullptr)==noErr);
        for(size_t n=1;n<in.size()-1;++n)assert(in[n]==0);assert(in.front()==99 && in.back()==99);
        assert(api.DoIOOperation(driver,2,4,22,kAudioServerPlugInIOOperationWriteMix,137,&c,out.data()+1,nullptr)!=noErr && out==original);
        assert(api.StopIO(driver,2,22)==noErr && watch.joined==seed);
        assert(api.AddDeviceClient(driver,2,&ca)==noErr);
    }
    assert(watch.prepared==2 && notices==12 && Pro40SilenceNotificationFailures(driver)==12);
    assert(Pro40DuplexInputFailures(driver)==2 && Pro40DuplexOutputFailures(driver)==2);
    assert(api.RemoveDeviceClient(driver,2,&ca)==noErr && api.RemoveDeviceClient(driver,2,&cb)==noErr && api.Release(driver)==0);
    // No configured connection means refusal, never a nominal silent session.
    driver=static_cast<AudioServerPlugInDriverRef>(Pro40SilenceFactory(nullptr,kAudioServerPlugInTypeUUID));auto &empty=**driver;
    assert(empty.Initialize(driver,&h)==noErr && empty.AddDeviceClient(driver,2,&ca)==noErr);
    assert(empty.StartIO(driver,2,11)!=noErr && empty.Release(driver)==0);
    // An unpublished preparation with failed cleanup blocks further attempts.
    driver=static_cast<AudioServerPlugInDriverRef>(Pro40SilenceFactory(nullptr,kAudioServerPlugInTypeUUID));auto &failed=**driver;
    Watch rejected;rejected.activate=false;rejected.cleanupOK=false;
    assert(failed.Initialize(driver,&h)==noErr && Pro40DuplexConfigure(driver,make(rejected,owner)) && failed.AddDeviceClient(driver,2,&ca)==noErr);
    assert(failed.StartIO(driver,2,11)!=noErr && failed.StartIO(driver,2,11)!=noErr && rejected.prepared==1 && rejected.joined==1);
    assert(failed.Release(driver)==0);
    // Failed startup can have unresolved resources without returning a session.
    driver=static_cast<AudioServerPlugInDriverRef>(Pro40SilenceFactory(nullptr,kAudioServerPlugInTypeUUID));auto &absent=**driver;
    unsigned attempts=0;
    assert(absent.Initialize(driver,&h)==noErr && Pro40DuplexConfigure(driver,[&](uint64_t,uint32_t,uint32_t){++attempts;return HalDuplexPreparation{nullptr,false};}));
    assert(absent.AddDeviceClient(driver,2,&ca)==noErr && absent.StartIO(driver,2,11)!=noErr && absent.StartIO(driver,2,11)!=noErr && attempts==1);
    assert(absent.Release(driver)==0);
    // Final Release is externally quiescent; it still joins an owned session.
    driver=static_cast<AudioServerPlugInDriverRef>(Pro40SilenceFactory(nullptr,kAudioServerPlugInTypeUUID));auto &closing=**driver;
    Watch closingWatch;
    assert(closing.Initialize(driver,&h)==noErr && Pro40DuplexConfigure(driver,make(closingWatch,owner)));
    assert(closing.AddDeviceClient(driver,2,&ca)==noErr && closing.StartIO(driver,2,11)==noErr && closingWatch.joined==0);
    assert(closing.Release(driver)==0 && closingWatch.joined==1);
    // Actual exported callbacks race a final StopIO. Buffer pins and callback
    // objects remain owned until all callers join and final Release drains.
    driver=static_cast<AudioServerPlugInDriverRef>(Pro40SilenceFactory(nullptr,kAudioServerPlugInTypeUUID));auto &racing=**driver;
    Watch racingWatch;assert(racing.Initialize(driver,&h)==noErr && Pro40DuplexConfigure(driver,make(racingWatch,owner)));
    assert(racing.AddDeviceClient(driver,2,&ca)==noErr && racing.StartIO(driver,2,11)==noErr);
    std::atomic<bool> done{false};std::atomic<unsigned> reads{0};std::array<std::thread,4> readers;
    for(auto &t:readers)t=std::thread([&]{
        while(!done.load()) {
            auto c=F::cycle(37,0);std::array<float,137*8+2> samples;samples.fill(99);
            const auto result=racing.DoIOOperation(driver,2,3,11,kAudioServerPlugInIOOperationReadInput,137,&c,samples.data()+1,nullptr);
            assert(samples.front()==99 && samples.back()==99);
            if(result==noErr) {
                const bool silence=samples[1]==0;
                for(size_t f=0;f<137;++f)for(size_t ch=0;ch<8;++ch)
                    assert(samples[1+f*8+ch]==(silence?0:float(F::sample(37+f,ch))/8388608.f));
                ++reads;
            }
        }
    });
    while(reads.load()<100)std::this_thread::yield();
    assert(racing.StopIO(driver,2,11)==noErr);done=true;
    for(auto &t:readers)t.join();assert(racing.Release(driver)==0 && racingWatch.joined==1);
    std::puts("PASS: actual duplex HAL table, 8/12 formats/streams, 137-frame input and WriteMix output, canaries, shared clients/removal, reentrant failed notifications, source loss, two starts, absent backend, cleanup failure, final release and four callbacks racing stop");
}
