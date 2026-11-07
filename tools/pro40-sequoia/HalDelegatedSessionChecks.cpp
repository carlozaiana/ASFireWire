// SPDX-License-Identifier: Apache-2.0
#include "DuplexAudioFormat.hpp"
// Joined controller/HAL/source threads with generated packets. This exercises
// real audio transport and code-bound control, not actual worker supervision,
// CoreAudio hosting, FireWire hardware or analogue latency.
#include "HalDelegatedSession.hpp"
#include "HalSilenceDriver.hpp"
#include "HalSourceLossControl.hpp"
#include "controller-lab/ControllerDelegatedSession.hpp"
#include "DuplexGeneratedSource.hpp"
#include "NativeDuplexSourceConnection.hpp"
#include <cassert>
#include <cstdio>
using namespace Pro40Audio;
using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;
namespace H=DuplexHostWire;
namespace W=DuplexRendezvousWire;
struct Endpoint {
    NativeMachPort receive{MACH_PORT_RIGHT_RECEIVE},send{MACH_PORT_RIGHT_SEND};
    Endpoint(){assert(receive.allocate()==KERN_SUCCESS);assert(mach_port_insert_right(mach_task_self(),receive.get(),receive.get(),MACH_MSG_TYPE_MAKE_SEND)==KERN_SUCCESS);assert(send.adopt(receive.get())==KERN_SUCCESS);}
};
static void pollPause(){std::this_thread::sleep_for(100us);}
int main(int argc,char **argv) {
    assert(argc==2 && std::strlen(argv[1])==40);
    auto text=CFStringCreateWithFormat(nullptr,nullptr,CFSTR("cdhash H\"%s\""),argv[1]);SecRequirementRef raw=nullptr;
    assert(SecRequirementCreateWithString(text,0,&raw)==errSecSuccess);CFRelease(text);IdentityDetail::Owned<SecRequirementRef> code(raw);
    auto token=W::selfToken();assert(token);mach_timebase_info_data_t tb{};assert(mach_timebase_info(&tb)==KERN_SUCCESS);
    for(unsigned scenario=0;scenario<6;++scenario) {
        Endpoint listener,workerListener;
        const H::Values values{91,1,tb.numer,tb.denom};const DuplexLaunchBinding binding{19,73,{1,2,3}};
        const SourceFrameClock::Identity identity{19,1,7,tb.numer,tb.denom};
        // Fixture policy comes from the local source configuration, never an
        // incoming message. Native profile must supply nativeDuplexClockPolicy.
        auto fixture=std::make_unique<DuplexGeneratedSource>();auto policy=fixture->policy();fixture.reset();
        std::atomic<uint64_t> checked{0};Pro40Lab::WorkerDuplexReport report;
        std::atomic<bool> cancel{false},controllerFinished{false};
        std::thread controller([&]{
            std::unique_ptr<H::Request> request;
            {NativeMachReceived<512> message;assert(message.read(listener.receive.get(),1000,nativeAuditReceiveOptions));request=H::accept(message,listener.receive.get(),code.get(),tb.numer,tb.denom);assert(request);}
            Pro40Lab::ControllerDelegatedSession owner(std::move(request),identity,scenario==4?500ms:3000ms);
            std::thread worker([&]{
                DuplexMachConnector connector;assert(connector.start(workerListener.send.get(),*token,binding,Clock::now()+2s));
                auto state=DuplexMachConnector::State::waiting;
                while((state=connector.poll())==DuplexMachConnector::State::waiting)pollPause();
                assert(state==DuplexMachConnector::State::connected);
                auto source=std::make_unique<DuplexGeneratedSource>();auto descriptor=source->descriptor();
                auto output=std::make_unique<PlaybackPcmQueue<8192,duplexOutputChannels>>(binding.epoch);
                NativeDuplexSourceConnection<SustainedInputReceiver<>> connection(connector.takeChannel(),*source->receiver,*output,descriptor,policy,mach_absolute_time());
                auto due=Clock::now()+16ms;bool confirmed=false;
                while(!connection.closed()) {
                    if(Clock::now()>=due && !connection.stopRequested()){source->step();due+=16ms;}
                    connection.poll(mach_absolute_time());
                    if(connection.primed()) {
                        std::array<int32_t,8*duplexOutputChannels> samples;
                        while(output->written()-output->consumed()>=8) {
                            const auto first=output->consumed();assert(output->read(first,samples));
                            for(unsigned f=0;f<8;++f)for(unsigned c=0;c<duplexOutputChannels;++c)
                                assert(samples[f*duplexOutputChannels+c]==(first+f<512?0:Fixture::sample(first+f,c)));
                            assert(output->commit(first,8));checked=first+8;
                        }
                    }
                    if(connection.stopRequested() && !confirmed){assert(connection.confirmStopped(scenario!=1));confirmed=true;}
                    pollPause();
                }
                assert(confirmed);
            });
            std::unique_ptr<DuplexEndpointGrant> grant;DuplexMachAcceptor acceptor;
            {NativeMachReceived<512> message;assert(message.read(workerListener.receive.get(),1000,nativeAuditReceiveOptions));grant=acceptor.prepare(message,workerListener.receive.get(),*token,binding,Clock::now()+2s);assert(grant);}
            report=owner.run(std::move(grant),*token,binding,cancel);
            worker.join();assert(owner.finishAfterJoin(scenario!=2 && scenario!=5));controllerFinished=true;
        });
        struct Host {AudioServerPlugInHostInterface table{};std::atomic<unsigned> requests{0};} hostState;
        auto &hostTable=hostState.table;
        hostTable.PropertiesChanged=[](AudioServerPlugInHostRef,AudioObjectID,UInt32,const AudioObjectPropertyAddress *)->OSStatus{return noErr;};
        hostTable.RequestDeviceConfigurationChange=[](AudioServerPlugInHostRef ref,AudioObjectID object,UInt64 action,void *info)->OSStatus {
            auto &h=*reinterpret_cast<Host *>(const_cast<AudioServerPlugInHostInterface *>(ref));
            assert(object==2 && action==HalSourceLossControl::action && !info);++h.requests;return noErr;
        };
        auto driver=static_cast<AudioServerPlugInDriverRef>(Pro40SilenceFactory(nullptr,kAudioServerPlugInTypeUUID));assert(driver);
        auto &api=**driver;assert(api.Initialize(driver,&hostTable)==noErr);
        AudioServerPlugInClientInfo client{};client.mClientID=11;assert(api.AddDeviceClient(driver,2,&client)==noErr);
        HalDuplexSession *session=nullptr;unsigned starts=0;
        assert(Pro40DuplexConfigure(driver,[&](uint64_t seed,uint32_t numer,uint32_t denom){
            ++starts;assert(seed==values.seed && numer==values.numer && denom==values.denom);
            NativeMachPort port(MACH_PORT_RIGHT_SEND);assert(port.retainSend(listener.send.get())==KERN_SUCCESS);
            auto introduction=std::make_unique<DuplexProvisionRegistry::Introduction>(std::move(port),*token,11);
            auto prepared=HalDelegatedSession::prepare(std::move(introduction),values,policy);
            assert(prepared.session && prepared.cleanupOK);session=prepared.session.get();return prepared;
        }));
        assert(api.StartIO(driver,2,11)==noErr && session && starts==1);
        const auto descriptor=session->descriptor;const auto inputFirst=descriptor.inputFirst+37;
        uint64_t outputFirst=descriptor.outputFirst+descriptor.primedOutputFrames;
        std::array<float,137*8> input{};std::array<float,137*duplexOutputChannels> output{};
        auto due=Clock::now();
        for(unsigned block=0;block<100;++block) {
            AudioServerPlugInIOCycleInfo cycle{};cycle.mInputTime.mFlags=kAudioTimeStampSampleTimeValid;
            cycle.mInputTime.mSampleTime=double(inputFirst+block*137);
            assert(api.DoIOOperation(driver,2,3,11,kAudioServerPlugInIOOperationReadInput,137,&cycle,input.data(),nullptr)==noErr && !Pro40DuplexInputFailures(driver));
            for(unsigned f=0;f<137;++f)for(unsigned c=0;c<8;++c)assert(input[f*8+c]==float(Fixture::sample(inputFirst+block*137+f,c))/8388608.f);
            for(unsigned f=0;f<137;++f)for(unsigned c=0;c<duplexOutputChannels;++c)output[f*duplexOutputChannels+c]=float(Fixture::sample(outputFirst+f,c))/8388608.f;
            cycle.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;cycle.mOutputTime.mSampleTime=double(outputFirst+descriptor.outputTimeline.callbackSample);
            assert(api.DoIOOperation(driver,2,4,11,kAudioServerPlugInIOOperationWriteMix,137,&cycle,output.data(),nullptr)==noErr);outputFirst+=137;
            due+=2854167ns;std::this_thread::sleep_until(due);
        }
        if(scenario>=3) {
            if(scenario!=4)cancel=true;
            // The application deliberately does NOT call StopIO until the
            // controller has its RELEASED and sends the independent receipt.
            const auto deadline=Clock::now()+3s;
            while(!controllerFinished.load() || !hostState.requests.load()) {assert(Clock::now()<deadline);pollPause();}
            assert(hostState.requests==1);
        }
        assert(api.StopIO(driver,2,11)==noErr);
        if(scenario>=3)assert(api.PerformDeviceConfigurationChange(driver,2,HalSourceLossControl::action,nullptr)==noErr);
        if(scenario)assert(api.StartIO(driver,2,11)!=noErr && starts==1); // Failed cleanup blocks the factory itself.
        assert(api.RemoveDeviceClient(driver,2,&client)==noErr);assert(api.Release(driver)==0);
        controller.join();assert(report.finished && report.localClosed && report.admitted);
        assert(report.peerClean==(scenario!=1) && checked>13000);
        assert(report.cancelled==(scenario>=3));
        assert(hostState.requests==(scenario>=3?1u:0u));
        std::printf("PASS: delegated generated8/12 session scenario=%u frames=%llu exported HAL IO table, native STOPPED and independent controller cleanup joined\n",scenario,(unsigned long long)checked.load());
    }
}
