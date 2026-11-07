// SPDX-License-Identifier: Apache-2.0
#include "HalSilenceDriver.hpp"
#include "HalSilenceState.hpp"
#include <array>
#include <cassert>
#include <thread>
#include <cstdio>
namespace {
AudioServerPlugInDriverRef observedDriver=nullptr;
UInt64 notifications=0;
bool failNotification=false;
OSStatus propertiesChanged(AudioServerPlugInHostRef,AudioObjectID object,UInt32 count,const AudioObjectPropertyAddress *addresses) {
    assert(count==1 && addresses);
    assert((object==2 && addresses->mSelector==kAudioDevicePropertyDeviceIsRunning) ||
           (object==3 && addresses->mSelector==kAudioStreamPropertyIsActive));
    assert(addresses->mScope==kAudioObjectPropertyScopeGlobal && addresses->mElement==kAudioObjectPropertyElementMain);
    UInt32 value=9,used=0;auto &api=**observedDriver;
    assert(api.GetPropertyData(observedDriver,object,0,addresses,0,nullptr,sizeof(value),&used,&value)==noErr);
    assert(used==sizeof(value) && value<=1);
    Float64 sample=0;UInt64 stamp=0,seed=0;
    assert((api.GetZeroTimeStamp(observedDriver,2,1,&sample,&stamp,&seed)==noErr)==(value==1));
    // Reenter a control operation too: this deadlocks if notification holds the
    // state mutex. Adding/removing a stopped client must not notify recursively.
    AudioServerPlugInClientInfo transient{};transient.mClientID=99;
    assert(api.AddDeviceClient(observedDriver,2,&transient)==noErr);
    assert(api.RemoveDeviceClient(observedDriver,2,&transient)==noErr);
    ++notifications;
    return failNotification?kAudioHardwareUnspecifiedError:OSStatus(noErr);
}
}
int main() {
    assert(!Pro40SilenceFactory(nullptr,IUnknownUUID));
    auto driver=static_cast<AudioServerPlugInDriverRef>(Pro40SilenceFactory(nullptr,kAudioServerPlugInTypeUUID));assert(driver);
    auto &api=**driver;
    assert(api.Initialize && api.HasProperty && api.BeginIOOperation && api.EndIOOperation && api.CreateDevice && api.AbortDeviceConfigurationChange);
    void *queried=reinterpret_cast<void *>(uintptr_t(1));CFUUIDBytes unknown{};
    assert(api.QueryInterface(driver,unknown,&queried)==E_NOINTERFACE && queried==nullptr);
    assert(api.QueryInterface(driver,CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID),&queried)==S_OK && queried==driver);
    assert(api.Release(queried)==1);
    assert(api.QueryInterface(driver,CFUUIDGetUUIDBytes(IUnknownUUID),&queried)==S_OK && queried==driver);
    assert(api.Release(queried)==1);
    assert(api.StartIO(driver,2,1)!=noErr);
    AudioServerPlugInHostInterface host{},otherHost{};
    assert(api.Initialize(driver,&host)!=noErr);
    observedDriver=driver;host.PropertiesChanged=propertiesChanged;
    otherHost.PropertiesChanged=propertiesChanged;
    assert(api.Initialize(driver,&host)==noErr && api.Initialize(driver,&host)==noErr);
    assert(api.Initialize(driver,&otherHost)!=noErr);
    // Real HAL registers discovery observers before the eventual IO client.
    // These idle registrations must not consume all available client slots.
    for(UInt32 id=1000;id<1032;++id) {
        AudioServerPlugInClientInfo observer{};observer.mClientID=id;
        assert(api.AddDeviceClient(driver,2,&observer)==noErr);
    }
    AudioServerPlugInClientInfo client{};client.mClientID=1;
    assert(api.AddDeviceClient(driver,2,&client)==noErr && api.StartIO(driver,2,1)==noErr);
    assert(notifications==2);
    assert(api.StartIO(driver,2,1)==noErr && notifications==2);
    assert(api.StopIO(driver,2,1)==noErr && notifications==2); // Balance second device hold.
    AudioServerPlugInClientInfo second{};second.mClientID=2;
    assert(api.AddDeviceClient(driver,2,&second)==noErr);
    // Sequoia asks a registered B for the shared device clock before Start B.
    Float64 sharedSample=0;UInt64 sharedHost=0,sharedSeed=0;
    assert(api.GetZeroTimeStamp(driver,2,2,&sharedSample,&sharedHost,&sharedSeed)==noErr && sharedSeed==1);
    assert(api.GetZeroTimeStamp(driver,2,999,&sharedSample,&sharedHost,&sharedSeed)!=noErr);
    assert(api.StartIO(driver,2,2)==noErr);
    assert(api.StopIO(driver,2,2)==noErr && api.RemoveDeviceClient(driver,2,&second)==noErr && notifications==2);
    AudioObjectPropertyAddress rate{kAudioDevicePropertyNominalSampleRate,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    Float64 sampleRate=0;UInt32 used=0;
    assert(api.GetPropertyData(driver,2,0,&rate,0,nullptr,sizeof(sampleRate),&used,&sampleRate)==noErr && sampleRate==48000 && used==8);
    Float64 sample=0;UInt64 stamp=0,seed=0;
    assert(api.GetZeroTimeStamp(driver,2,1,&sample,&stamp,&seed)==noErr && seed==1);
    AudioServerPlugInIOCycleInfo initialCycle{};
    assert(api.BeginIOOperation(driver,2,1,kAudioServerPlugInIOOperationReadInput,64,&initialCycle)==noErr);
    assert(api.EndIOOperation(driver,2,1,kAudioServerPlugInIOOperationReadInput,64,&initialCycle)==noErr);
    assert(api.BeginIOOperation(driver,2,1,0,64,&initialCycle)!=noErr);
    // Keep root reference for the complete threaded test; each caller owns an
    // additional reference until its final callback. Final destruction after join.
    std::array<std::thread,4> readers;
    std::atomic<unsigned> ready{0};std::atomic<bool> done{false};
    std::array<uint64_t,4> successes{};
    for(size_t n=0;n<readers.size();++n) {
        assert(api.AddRef(driver)>=2);
        readers[n]=std::thread([&,n]{
            AudioServerPlugInIOCycleInfo cycle{};std::array<Float32,66> samples;
            UInt64 lastSeed=0;Float64 lastSample=0;
            do {
                samples.fill(99);
                const auto status=api.DoIOOperation(driver,2,3,1,kAudioServerPlugInIOOperationReadInput,64,&cycle,samples.data()+1,nullptr);
                assert(samples.front()==99 && samples.back()==99);
                if(!status) {
                    for(size_t j=1;j<65;++j)assert(samples[j]==0);
                    if(++successes[n]==1)ready.fetch_add(1);
                } else for(auto v:samples)assert(v==99);
                Float64 sampleTime=0;UInt64 hostTime=0,clockSeed=0;
                if(api.GetZeroTimeStamp(driver,2,1,&sampleTime,&hostTime,&clockSeed)==noErr) {
                    assert(clockSeed>=lastSeed && sampleTime>=0 && uint64_t(sampleTime)%Pro40Audio::HalSilenceModel::timestampPeriod==0);
                    if(clockSeed==lastSeed)assert(sampleTime>=lastSample);
                    lastSeed=clockSeed;lastSample=sampleTime;
                }
            } while(!done.load());
            api.Release(driver);
        });
    }
    while(ready.load()!=4)std::this_thread::yield();
    for(unsigned n=0;n<10000;++n) {
        assert(api.StopIO(driver,2,1)==noErr);
        assert(api.StartIO(driver,2,1)==noErr);
    }
    done.store(true);for(auto &reader:readers)reader.join();
    assert(api.GetZeroTimeStamp(driver,2,1,&sample,&stamp,&seed)==noErr && seed==10001);
    assert(api.StopIO(driver,2,1)==noErr && api.RemoveDeviceClient(driver,2,&client)==noErr);
    assert(notifications==40004 && Pro40SilenceNotificationFailures(driver)==0);
    failNotification=true;
    assert(api.AddDeviceClient(driver,2,&client)==noErr && api.StartIO(driver,2,1)==noErr);
    assert(api.GetZeroTimeStamp(driver,2,1,&sample,&stamp,&seed)==noErr);
    assert(api.StopIO(driver,2,1)==noErr && api.RemoveDeviceClient(driver,2,&client)==noErr);
    assert(notifications==40008 && Pro40SilenceNotificationFailures(driver)==4);
    failNotification=false;
    // Replay the real remote-host sequence: A starts, B joins without Start B,
    // A disappears, B still reads, final Stop comes under B's registered ID.
    assert(api.AddDeviceClient(driver,2,&client)==noErr && api.StartIO(driver,2,1)==noErr);
    assert(api.AddDeviceClient(driver,2,&second)==noErr);
    std::array<Float32,64> sharedAudio;sharedAudio.fill(9);
    assert(api.DoIOOperation(driver,2,3,2,kAudioServerPlugInIOOperationReadInput,64,&initialCycle,sharedAudio.data(),nullptr)==noErr);
    const auto sharedEpoch=seed;
    assert(api.GetZeroTimeStamp(driver,2,2,&sample,&stamp,&seed)==noErr && seed>sharedEpoch);
    const auto beforeRemove=seed;
    assert(api.RemoveDeviceClient(driver,2,&client)==noErr);
    assert(api.GetZeroTimeStamp(driver,2,2,&sample,&stamp,&seed)==noErr && seed==beforeRemove);
    assert(api.DoIOOperation(driver,2,3,2,kAudioServerPlugInIOOperationReadInput,64,&initialCycle,sharedAudio.data(),nullptr)==noErr);
    assert(api.DoIOOperation(driver,2,3,1,kAudioServerPlugInIOOperationReadInput,64,&initialCycle,sharedAudio.data(),nullptr)!=noErr);
    assert(api.StopIO(driver,2,2)==noErr);
    assert(api.StopIO(driver,2,2)!=noErr); // No count underflow.
    assert(api.GetZeroTimeStamp(driver,2,2,&sample,&stamp,&seed)!=noErr);
    assert(api.RemoveDeviceClient(driver,2,&second)==noErr);
    assert(api.Release(driver)==0); // Sanitizers check final deletion after all callers.
    // Independent atomic-snapshot stress: active state, anchor and seed must
    // belong to exactly the same epoch, including while clients transition.
    using State=Pro40Audio::HalSilenceState;
    State state(1,1);assert(state.change(State::Change::add,1));
    std::atomic<bool> finished{false};
    std::thread observer([&]{Pro40Audio::HalSilenceModel::Snapshot s;
        while(!finished.load())if(state.read(s) && s.active(1))assert(s.anchor==s.seed*1000);
    });
    for(uint64_t n=1;n<=20000;++n) {
        assert(state.change(State::Change::start,1,n*1000));
        assert(state.change(State::Change::stop,1));
    }
    finished.store(true);observer.join();
    std::puts("PASS: factory/ref lifetime, reentrant host notifications, notification failures preserve IO state, four concurrent readers and coherent snapshots. No HAL host installed.");
}
