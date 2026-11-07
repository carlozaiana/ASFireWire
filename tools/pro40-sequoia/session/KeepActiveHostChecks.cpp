// SPDX-License-Identifier: Apache-2.0
// Bounded silent host check; records buffer metadata, never microphone samples.
#define main keepActiveEntry
#include "KeepActive.cpp"
#undef main
std::atomic<unsigned> observations{0};
OSStatus observe(AudioObjectID id,const AudioTimeStamp *now,const AudioBufferList *input,const AudioTimeStamp *it,
                 AudioBufferList *output,const AudioTimeStamp *ot,void *context) {
    unsigned bits=0;
    if(input && input->mNumberBuffers==1)bits|=1;
    if(input && input->mNumberBuffers==1 && input->mBuffers[0].mData)bits|=2;
    if(output && output->mNumberBuffers==1) {
        bits|=4;const auto &b=output->mBuffers[0];
        if(b.mNumberChannels==12)bits|=8;
        if(b.mData)bits|=16;
        if(b.mDataByteSize && b.mDataByteSize%(12*sizeof(float))==0 && b.mDataByteSize<=4096*12*sizeof(float))bits|=32;
    }
    observations.fetch_or(bits,std::memory_order_relaxed);
    return io(id,now,input,it,output,ot,context);
}
int main() {
    const auto id=find();if(!id || !inspect(id))return 1;
    AudioDeviceIOProcID proc=nullptr;
    if(AudioDeviceCreateIOProcID(id,observe,nullptr,&proc) || !proc)return 1;
    const bool disabled=disableInput(id,proc);
    const auto started=disabled?AudioDeviceStart(id,proc):kAudioHardwareIllegalOperationError;
    if(!started)std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto stopped=started?noErr:AudioDeviceStop(id,proc);
    const auto destroyed=AudioDeviceDestroyIOProcID(id,proc);
    std::printf("input-disabled=%u start=%d stop=%d destroy=%d calls=%llu failures=%llu observed-mask=%u (input-list=1 input-pointer=2 output-list=4 output-6ch=8 output-pointer=16 output-size=32)\n",
        unsigned(disabled),int(started),int(stopped),int(destroyed),(unsigned long long)callbacks.load(),
        (unsigned long long)failures.load(),observations.load());
    return started || stopped || destroyed || !callbacks.load() || failures.load() ||
        (observations.load()&60)!=60?1:0;
}
