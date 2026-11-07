// SPDX-License-Identifier: Apache-2.0
#include "HalSpeakerLayout.hpp"
#include <cassert>
#include <cstdio>
#include <limits>
#include <thread>
using Speakers=Pro40Audio::HalSpeakerPreferences<6>;
namespace {
CFPropertyListRef storage=nullptr;
Speakers *current=nullptr;
AudioServerPlugInHostInterface host{};
bool failWrite=false,reenter=false;
unsigned writes=0;
OSStatus copy(AudioServerPlugInHostRef,CFStringRef,CFPropertyListRef *out) {
    *out=storage?CFRetain(storage):nullptr;return noErr;
}
OSStatus save(AudioServerPlugInHostRef h,CFStringRef,CFPropertyListRef data) {
    ++writes;
    if(reenter) {
        // Host storage may read back the old committed state. It must not
        // deadlock, nor allow a nested write to race the pending transaction.
        const auto before=current->snapshot();bool changed=true;
        assert(current->set(h,kAudioDevicePropertyPreferredChannelsForStereo,sizeof(before.stereo),before.stereo.data(),changed)==kAudioHardwareIllegalOperationError && !changed);
    }
    if(failWrite)return kAudioHardwareUnspecifiedError;
    if(storage)CFRelease(storage);storage=CFRetain(data);return noErr;
}
bool same(const Speakers::Snapshot &a,const Speakers::Snapshot &b) {
    return std::memcmp(&a.layout,&b.layout,sizeof(a.layout))==0 && a.stereo==b.stereo;
}
}
int main() {
    host.CopyFromStorage=copy;host.WriteToStorage=save;
    Speakers preferences;current=&preferences;
    auto original=Speakers::defaults();bool changed=false;
    assert(preferences.set(&host,kAudioDevicePropertyPreferredChannelLayout,sizeof(original.layout),&original.layout,changed)!=noErr);
    assert(preferences.initialize(&host) && same(preferences.snapshot(),original));
    auto reordered=original.layout;std::swap(reordered.descriptions[0],reordered.descriptions[5]);
    reenter=true;
    assert(preferences.set(&host,kAudioDevicePropertyPreferredChannelLayout,sizeof(reordered),&reordered,changed)==noErr && changed);
    assert(writes==1);
    assert(preferences.set(&host,kAudioDevicePropertyPreferredChannelLayout,sizeof(reordered),&reordered,changed)==noErr && !changed && writes==1);
    auto before=preferences.snapshot();
    for(auto pair:{std::array<UInt32,2>{1,1},{0,2},{1,7}})
        assert(preferences.set(&host,kAudioDevicePropertyPreferredChannelsForStereo,sizeof(pair),pair.data(),changed)!=noErr && !changed && same(before,preferences.snapshot()));
    std::array<UInt32,2> stereo{5,6};failWrite=true;
    assert(preferences.set(&host,kAudioDevicePropertyPreferredChannelsForStereo,sizeof(stereo),stereo.data(),changed)!=noErr && !changed && same(before,preferences.snapshot()));
    failWrite=false;
    assert(preferences.set(&host,kAudioDevicePropertyPreferredChannelsForStereo,sizeof(stereo),stereo.data(),changed)==noErr && changed);
    Speakers restarted;assert(restarted.initialize(&host) && same(restarted.snapshot(),preferences.snapshot()));
    auto invalid=reordered;invalid.descriptions[1]=invalid.descriptions[0];
    assert(preferences.set(&host,kAudioDevicePropertyPreferredChannelLayout,sizeof(invalid),&invalid,changed)!=noErr && !changed);
    invalid=reordered;invalid.descriptions[2].mChannelFlags=0x80000000;
    assert(preferences.set(&host,kAudioDevicePropertyPreferredChannelLayout,sizeof(invalid),&invalid,changed)!=noErr);
    invalid=reordered;invalid.descriptions[2].mChannelLabel=12345;
    assert(preferences.set(&host,kAudioDevicePropertyPreferredChannelLayout,sizeof(invalid),&invalid,changed)!=noErr);
    Speakers::Layout normalized;
    // Public readback after Audio MIDI Setup saves a map includes nominal
    // spherical coordinates. The channel roles must survive normalization.
    auto publicLayout=reordered;
    publicLayout.descriptions[0].mChannelFlags=kAudioChannelFlags_SphericalCoordinates;
    publicLayout.descriptions[0].mCoordinates[0]=110.f;
    publicLayout.descriptions[0].mCoordinates[2]=1.f;
    assert(Speakers::normalize(sizeof(publicLayout),&publicLayout,normalized)==noErr &&
           std::memcmp(&normalized,&reordered,sizeof(normalized))==0);
    publicLayout.descriptions[0].mCoordinates[0]=std::numeric_limits<float>::quiet_NaN();
    assert(Speakers::normalize(sizeof(publicLayout),&publicLayout,normalized)!=noErr);
    for(auto tag:{kAudioChannelLayoutTag_MPEG_5_1_A,kAudioChannelLayoutTag_Stereo}) {
        std::array<UInt32,3> header{tag,0,0};
        assert(Speakers::normalize(sizeof(header),header.data(),normalized)==noErr);
        if(tag==kAudioChannelLayoutTag_Stereo)for(unsigned i=2;i<6;++i)assert(normalized.descriptions[i].mChannelLabel==kAudioChannelLabel_Unused);
        else assert(std::memcmp(&normalized,&original.layout,sizeof(normalized))==0);
    }
    const std::array<UInt32,3> bitmap{kAudioChannelLayoutTag_UseChannelBitmap,63,0};
    assert(Speakers::normalize(sizeof(bitmap),bitmap.data(),normalized)==noErr && std::memcmp(&normalized,&original.layout,sizeof(normalized))==0);
    const std::array<UInt32,3> tooMany{kAudioChannelLayoutTag_MPEG_7_1_A,0,0};
    assert(Speakers::normalize(sizeof(tooMany),tooMany.data(),normalized)!=noErr);
    for(UInt32 size=0;size<sizeof(original.layout);++size)
        assert(Speakers::normalize(size,&original.layout,normalized)!=noErr);
    std::array<unsigned char,sizeof(original.layout)+2> bytes;bytes.fill(0x7f);UInt32 used=99;
    assert(preferences.get(kAudioDevicePropertyPreferredChannelLayout,sizeof(original.layout)-1,&used,bytes.data()+1)!=noErr && used==0);
    for(auto b:bytes)assert(b==0x7f);
    assert(preferences.get(kAudioDevicePropertyPreferredChannelLayout,sizeof(original.layout),&used,bytes.data()+1)==noErr && used==sizeof(original.layout));
    assert(bytes.front()==0x7f && bytes.back()==0x7f);
    // Concurrent property readers see whole snapshots while a control thread
    // saves assignments. These locks are never used by the audio callback.
    reenter=false;std::atomic<bool> done{false};
    std::thread reader([&]{while(!done.load()){auto s=preferences.snapshot();assert(Speakers::validLayout(s.layout) && Speakers::validStereo(s.stereo));}});
    for(unsigned i=0;i<1000;++i) {
        auto &layout=i%2?reordered:original.layout;
        assert(preferences.set(&host,kAudioDevicePropertyPreferredChannelLayout,sizeof(layout),&layout,changed)==noErr);
    }
    done.store(true);reader.join();
    // Upgrade real saved six-channel assignments, including a custom stereo
    // pair, without writing storage during discovery. New outputs remain free.
    using Twelve=Pro40Audio::HalSpeakerPreferences<12>;
    const auto legacy=preferences.snapshot();const auto beforeUpgradeWrites=writes;
    Twelve expanded;assert(expanded.initialize(&host));
    const auto upgraded=expanded.snapshot();
    assert(upgraded.stereo==legacy.stereo && writes==beforeUpgradeWrites);
    for(unsigned i=0;i<6;++i)assert(upgraded.layout.descriptions[i].mChannelLabel==legacy.layout.descriptions[i].mChannelLabel);
    for(unsigned i=6;i<12;++i)assert(upgraded.layout.descriptions[i].mChannelLabel==kAudioChannelLabel_Unused);
    Twelve::Layout normalizedTwelve;
    assert(Twelve::normalize(sizeof(legacy.layout),&legacy.layout,normalizedTwelve)==noErr);
    assert(std::memcmp(&normalizedTwelve,&upgraded.layout,sizeof(normalizedTwelve))==0);
    auto twelveLayout=upgraded.layout;
    for(unsigned i=6;i<12;++i)twelveLayout.descriptions[i].mChannelLabel=kAudioChannelLabel_Discrete_0+i;
    assert(expanded.set(&host,kAudioDevicePropertyPreferredChannelLayout,sizeof(twelveLayout),&twelveLayout,changed)==noErr && changed);
    const std::array<UInt32,2> lastPair{11,12};
    assert(expanded.set(&host,kAudioDevicePropertyPreferredChannelsForStereo,sizeof(lastPair),lastPair.data(),changed)==noErr && changed);
    Twelve restoredTwelve;assert(restoredTwelve.initialize(&host));
    const auto restoredState=restoredTwelve.snapshot();
    assert(restoredState.stereo==lastPair && std::memcmp(&restoredState.layout,&twelveLayout,sizeof(twelveLayout))==0);
    const std::array<UInt32,2> beyond{1,13};
    assert(expanded.set(&host,kAudioDevicePropertyPreferredChannelsForStereo,sizeof(beyond),beyond.data(),changed)!=noErr && !changed);
    // Invalid stored preferences fall back without deleting or rewriting them.
    CFRelease(storage);storage=CFRetain(CFSTR("not a preference dictionary"));
    const auto savedWrites=writes;Speakers corrupt;
    assert(!corrupt.initialize(&host) && same(corrupt.snapshot(),original) && writes==savedWrites);
    assert(Twelve::validLayout(Twelve::defaults().layout));
    CFRelease(storage);storage=nullptr;
    std::puts("PASS explicit assignments, tag/bitmap expansion, stereo validation, malformed/duplicate rejection, transactional storage, reload, reentrant host and concurrent reads; no audio device opened");
}
