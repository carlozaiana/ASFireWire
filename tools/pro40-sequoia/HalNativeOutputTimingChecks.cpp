// SPDX-License-Identifier: Apache-2.0
#include "DuplexAudioFormat.hpp"
#include "AudioCoreAudioWrite.hpp"
#include "NativeDuplexPolicy.hpp"
#include "PlaybackStartupGate.hpp"
#include <cassert>
#include <cstdio>
#include <memory>
using namespace Pro40Audio;
namespace {
constexpr unsigned block=512,bank=1536;
int32_t pattern(uint64_t frame,unsigned channel){return int32_t((frame%8192)*64+channel*4096)-300000;}
enum class Failure {none,callback,native};
struct Result {Failure failure;uint64_t low,high;};
// Conditional model, with separate preparation/presentation axes. In the latest
// hardware trace the next prepared packet presents about52ms after the offer;
// sweep42..67ms instead of treating that observation as a fixed device constant.
// The actual aged-origin HAL run starts callbacks13.4ms after StartIO. Model
// first waits13.3/21.3ms plus independent20ms lateness,40ms admission and10ms IPC.
// Real source-clock qualification and input history have separate checks.
Result simulate(unsigned prepAhead,unsigned phase,unsigned admission,unsigned firstWait,
                unsigned late,unsigned delivery,unsigned nativeLate,unsigned safety,unsigned seconds) {
    auto host=std::make_unique<PlaybackPcmQueue<8192,duplexOutputChannels>>(19),native=std::make_unique<PlaybackPcmQueue<8192,duplexOutputChannels>>(19);
    BasicPlaybackStartupGate<duplexOutputChannels> gate(*native);assert(gate.schedule(nativeDuplexLeadFrames,nativeDuplexPrimeFrames));
    const int64_t clockAtOffer=7200; // Absolute sample0 predates this offer150ms.
    const int64_t atCutover=clockAtOffer+prepAhead+nativeDuplexLeadFrames;
    const AudioCallbackTimeline map{19,1,0,atCutover};
    const uint64_t firstPresentation=clockAtOffer+admission+firstWait+safety+block;
    const int64_t firstFeed=int64_t(firstPresentation)-atCutover;
    std::array<int32_t,1024*duplexOutputChannels> prime{};std::array<float,block*duplexOutputChannels> samples{};
    std::array<int32_t,512*duplexOutputChannels> transfer{};std::array<int32_t,8*duplexOutputChannels> packet{};
    uint64_t number=0,nextDelivery=admission+delivery;unsigned banks=0;bool admitted=false,armed=false;
    const auto result=[&](Failure failure){return Result{failure,native->lowWater(),native->highWater()};};
    for(uint64_t time=0;time<uint64_t(seconds)*48000;++time) {
        if(time==admission){assert(host->write(19,0,prime)==PlaybackQueueResult::ok);admitted=true;}
        while(admitted && admission+firstWait+number*block+(number%23==0?late:0)<=time) {
            AudioServerPlugInIOCycleInfo cycle{};cycle.mNominalIOBufferFrameSize=block;
            cycle.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;
            cycle.mOutputTime.mSampleTime=double(firstPresentation+number*block);
            for(unsigned f=0;f<block;++f)for(unsigned c=0;c<duplexOutputChannels;++c)
                samples[f*duplexOutputChannels+c]=float(pattern(firstPresentation+number*block+f,c))/8388608.f;
            if(!writeCoreAudioOutput(map,*host,19,1,cycle,block,samples,number==0).ok())return result(Failure::callback);
            ++number;
        }
        if(admitted && time>=nextDelivery) {
            for(unsigned n=0;n<8;++n) {
                const auto count=std::min<uint64_t>({512,host->written()-host->consumed(),native->writable()})/8*8;
                if(!count)break;
                const auto first=host->consumed();auto data=std::span(transfer).first(count*duplexOutputChannels);
                assert(host->read(first,data) && native->write(19,first,data)==PlaybackQueueResult::ok && host->commit(first,count));
            }
            if(!armed && native->written()>=nativeDuplexPrimeFrames){assert(gate.arm());armed=true;}
            nextDelivery=time+std::max(1u,delivery);
        }
        while(time>=phase+uint64_t(banks)*bank+(banks%17==0?nativeLate:0)) {
            ++banks;
            for(unsigned frame=0;frame<bank;frame+=8) {
                const auto first=gate.nextNativeFrame();
                if(!gate.read(first,packet))return result(Failure::native);
                for(unsigned f=0;f<8;++f)for(unsigned c=0;c<duplexOutputChannels;++c) {
                    const int64_t feed=int64_t(first+f)-int64_t(nativeDuplexLeadFrames);
                    const bool application=feed>=0 && feed>=firstFeed;
                    assert(packet[f*duplexOutputChannels+c]==(application?pattern(uint64_t(atCutover+feed),c):0));
                }
                assert(gate.commit(first,8));
            }
        }
    }
    assert(gate.startedAt()==nativeDuplexLeadFrames && number>0);
    return result(Failure::none);
}
}
int main() {
    unsigned cases=0;uint64_t low=UINT64_MAX,high=0;
    for(unsigned prep:{2000u,2500u,3200u})for(unsigned phase:{0u,768u,1535u})
      for(unsigned admission:{0u,1920u})for(unsigned firstWait:{640u,1024u})
        for(unsigned late:{0u,960u})for(unsigned delivery:{0u,480u})for(unsigned nativeLate:{0u,960u}) {
            const auto r=simulate(prep,phase,admission,firstWait,late,delivery,nativeLate,8192,2);
            if(r.failure!=Failure::none)std::fprintf(stderr,"failed prep=%u phase=%u admission=%u first=%u late=%u delivery=%u native=%u why=%u\n",prep,phase,admission,firstWait,late,delivery,nativeLate,unsigned(r.failure));
            assert(r.failure==Failure::none);low=std::min(low,r.low);high=std::max(high,r.high);++cases;
        }
    assert(simulate(2500,0,1920,1024,960,480,960,8192,90).failure==Failure::none);
    assert(simulate(3200,0,0,640,0,0,0,0,2).failure==Failure::callback); // Too early for physical cutover.
    assert(simulate(2000,0,1920,1024,0,0,0,16384,2).failure==Failure::callback); // Initial gap exceeds capacity.
    assert(simulate(2500,0,1920,3328,960,480,0,8192,2).failure==Failure::native); // Newly anchored clock69ms wait.
    std::printf("PASS: %u CoreAudio presentation/preparation/startup/jitter cases +90s; native queue low/high=%llu/%llu; zero/oversized safety and69ms first wait rejected\n",cases,(unsigned long long)low,(unsigned long long)high);
}
