// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <CoreAudio/AudioServerPlugIn.h>
#include <AudioToolbox/AudioFormat.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <mutex>

namespace Pro40Audio {
// AudioHardwareBase.h: preferred layout describes the role of each physical
// channel. This is metadata, never a permutation of the samples sent to hardware.
// CoreAudioBaseTypes.h: AudioChannelLayout has a 12-byte header followed by
// mNumberChannelDescriptions entries. Always publish explicit descriptions:
// Sequoia Audio MIDI Setup showed every assignment as '-' for our tagged layout.
template<size_t Channels> struct SpeakerLayout {
    AudioChannelLayoutTag tag=kAudioChannelLayoutTag_UseChannelDescriptions;
    AudioChannelBitmap bitmap=0;
    UInt32 count=Channels;
    std::array<AudioChannelDescription,Channels> descriptions{};
};
static_assert(offsetof(SpeakerLayout<6>,descriptions)==offsetof(AudioChannelLayout,mChannelDescriptions));
static_assert(sizeof(SpeakerLayout<6>)==12+6*sizeof(AudioChannelDescription));

template<size_t Channels> class HalSpeakerPreferences {
    static_assert(Channels>=2 && Channels<=12);
public:
    using Layout=SpeakerLayout<Channels>;
    struct Snapshot {Layout layout;std::array<UInt32,2> stereo{1,2};};
    static Snapshot defaults() {
        Snapshot result;
        constexpr AudioChannelLabel surround[]={kAudioChannelLabel_Left,kAudioChannelLabel_Right,
            kAudioChannelLabel_Center,kAudioChannelLabel_LFEScreen,
            kAudioChannelLabel_LeftSurround,kAudioChannelLabel_RightSurround};
        for(size_t i=0;i<Channels;++i)
            result.layout.descriptions[i].mChannelLabel=i<(Channels>=6?6:2)?surround[i]:AudioChannelLabel(Channels>6?kAudioChannelLabel_Unused:kAudioChannelLabel_Discrete_0+i);
        return result;
    }
    static bool handles(AudioObjectID object,const AudioObjectPropertyAddress &a) {
        return object==2 && a.mScope==kAudioObjectPropertyScopeOutput && a.mElement==kAudioObjectPropertyElementMain &&
            (a.mSelector==kAudioDevicePropertyPreferredChannelLayout || a.mSelector==kAudioDevicePropertyPreferredChannelsForStereo);
    }
    static bool validStereo(const std::array<UInt32,2> &v) {
        return v[0]>=1 && v[0]<=Channels && v[1]>=1 && v[1]<=Channels && v[0]!=v[1];
    }
    static bool validLayout(const Layout &v) {
        if(v.tag!=kAudioChannelLayoutTag_UseChannelDescriptions || v.bitmap || v.count!=Channels)return false;
        for(size_t i=0;i<Channels;++i) {
            const auto &d=v.descriptions[i];const auto label=d.mChannelLabel;
            const bool unnamed=label==kAudioChannelLabel_Unused || label==kAudioChannelLabel_Unknown || label==kAudioChannelLabel_Discrete;
            // Defined speaker positions (CoreAudioBaseTypes.h), plus numbered
            // discrete outputs. Coordinates, ambisonic and object layouts are
            // not speaker assignments and are intentionally not accepted here.
            const bool speaker=(label>=1 && label<=18) || (label>=33 && label<=45) || label==49 || (label>=51 && label<=66);
            const bool discrete=label>=kAudioChannelLabel_Discrete_0 && label<kAudioChannelLabel_Discrete_0+Channels;
            if((!unnamed && !speaker && !discrete) || d.mChannelFlags ||
               d.mCoordinates[0]!=0 || d.mCoordinates[1]!=0 || d.mCoordinates[2]!=0)return false;
            if(!unnamed)for(size_t j=0;j<i;++j)if(v.descriptions[j].mChannelLabel==label)return false;
        }
        return true;
    }
    static OSStatus normalize(UInt32 size,const void *data,Layout &out) {
        constexpr UInt32 header=offsetof(AudioChannelLayout,mChannelDescriptions);
        if(!data || size<header)return kAudioHardwareBadPropertySizeError;
        std::array<UInt32,3> h{};std::memcpy(h.data(),data,header);
        Layout candidate;
        if(h[0]==kAudioChannelLayoutTag_UseChannelDescriptions) {
            if(h[1] || !h[2] || h[2]>Channels)return kAudioHardwareIllegalOperationError;
            if(size!=header+h[2]*sizeof(AudioChannelDescription))return kAudioHardwareBadPropertySizeError;
            std::memcpy(candidate.descriptions.data(),static_cast<const unsigned char *>(data)+header,h[2]*sizeof(AudioChannelDescription));
        } else {
            if(size!=header || h[2] || (h[0]!=kAudioChannelLayoutTag_UseChannelBitmap && h[1]))return kAudioHardwareBadPropertySizeError;
            // AudioFormat.h ChannelLayoutForTag/Bitmap supplies Apple's canonical
            // expansion. No hand-written mapping of encoded layout constants.
            const auto selector=h[0]==kAudioChannelLayoutTag_UseChannelBitmap?
                kAudioFormatProperty_ChannelLayoutForBitmap:kAudioFormatProperty_ChannelLayoutForTag;
            const UInt32 specifier=h[0]==kAudioChannelLayoutTag_UseChannelBitmap?h[1]:h[0];
            UInt32 expandedSize=0;
            if(AudioFormatGetPropertyInfo(selector,sizeof(specifier),&specifier,&expandedSize)!=noErr ||
               expandedSize<header || expandedSize>sizeof(Layout))return kAudioHardwareIllegalOperationError;
            Layout expanded;
            // On Sequoia, the converter fills descriptions but retains the
            // source tag/bitmap. Copy only those descriptions into our explicit
            // canonical header; requiring a rewritten tag rejects valid 5.1.
            if(AudioFormatGetProperty(selector,sizeof(specifier),&specifier,&expandedSize,&expanded)!=noErr ||
               !expanded.count || expanded.count>Channels || expandedSize!=header+expanded.count*sizeof(AudioChannelDescription))
                return kAudioHardwareIllegalOperationError;
            candidate.descriptions=expanded.descriptions;
        }
        // Apple's expansion also supplies nominal coordinates for named
        // speakers. Physical channel assignments use the labels; canonicalize
        // that optional geometry instead of rejecting Apple's own layouts.
        for(auto &d:candidate.descriptions) {
            constexpr UInt32 known=kAudioChannelFlags_RectangularCoordinates|kAudioChannelFlags_SphericalCoordinates|kAudioChannelFlags_Meters;
            if((d.mChannelFlags&~known) || !std::isfinite(d.mCoordinates[0]) ||
               !std::isfinite(d.mCoordinates[1]) || !std::isfinite(d.mCoordinates[2]))return kAudioHardwareIllegalOperationError;
            d.mChannelFlags=0;for(auto &coordinate:d.mCoordinates)coordinate=0;
        }
        if(!validLayout(candidate))return kAudioHardwareIllegalOperationError;
        out=candidate;return noErr;
    }
private:
    mutable std::mutex mutex_; // Property/control calls only; never in audio IO.
    std::atomic_flag writer_=ATOMIC_FLAG_INIT;
    std::atomic<bool> ready_{false};
    Snapshot state_=defaults();
    struct WriteGuard {std::atomic_flag &flag;~WriteGuard(){flag.clear(std::memory_order_release);}};
    static CFStringRef storageKey(){return CFSTR("OutputSpeakerPreferences.v1");}
    template<size_t N> static CFArrayRef array(const std::array<UInt32,N> &values) {
        auto result=CFArrayCreateMutable(nullptr,0,&kCFTypeArrayCallBacks);if(!result)return nullptr;
        for(auto value:values) {
            const int64_t n=value;auto number=CFNumberCreate(nullptr,kCFNumberSInt64Type,&n);
            if(!number){CFRelease(result);return nullptr;}
            CFArrayAppendValue(result,number);CFRelease(number);
        }
        return result;
    }
    template<size_t N> static bool decode(CFTypeRef object,std::array<UInt32,N> &out) {
        if(!object || CFGetTypeID(object)!=CFArrayGetTypeID())return false;
        auto a=static_cast<CFArrayRef>(object);if(CFArrayGetCount(a)!=N)return false;
        for(size_t i=0;i<N;++i) {
            auto number=CFArrayGetValueAtIndex(a,i);int64_t value=-1;
            if(!number || CFGetTypeID(number)!=CFNumberGetTypeID() ||
               CFNumberIsFloatType(static_cast<CFNumberRef>(number)) ||
               !CFNumberGetValue(static_cast<CFNumberRef>(number),kCFNumberSInt64Type,&value) || value<0 || value>UINT32_MAX)return false;
            out[i]=UInt32(value);
        }
        return true;
    }
    static CFDictionaryRef encode(const Snapshot &state) {
        std::array<UInt32,Channels> labels{};
        for(size_t i=0;i<Channels;++i)labels[i]=state.layout.descriptions[i].mChannelLabel;
        auto layout=array(labels),stereo=array(state.stereo);
        CFDictionaryRef result=nullptr;
        if(layout && stereo) {
            const void *keys[]={CFSTR("labels"),CFSTR("stereo")};const void *values[]={layout,stereo};
            result=CFDictionaryCreate(nullptr,keys,values,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        }
        if(layout)CFRelease(layout);if(stereo)CFRelease(stereo);return result;
    }
public:
    Snapshot snapshot() const {std::lock_guard lock(mutex_);return state_;}
    // AudioServerPlugIn.h CopyFromStorage returns an owned CFPropertyList.
    // A missing preference is normal on first install. Invalid stored contents
    // never become a channel map and are not overwritten during discovery.
    bool initialize(AudioServerPlugInHostRef host) {
        CFPropertyListRef saved=nullptr;bool valid=true;
        if(host && host->CopyFromStorage && host->CopyFromStorage(host,storageKey(),&saved)==noErr && saved) {
            Snapshot restored;std::array<UInt32,Channels> labels{};
            valid=CFGetTypeID(saved)==CFDictionaryGetTypeID();
            if(valid) {
                auto dict=static_cast<CFDictionaryRef>(saved);
                const auto storedLabels=CFDictionaryGetValue(dict,CFSTR("labels"));
                valid=decode(CFDictionaryGetValue(dict,CFSTR("stereo")),restored.stereo);
                if(valid && !decode(storedLabels,labels)) {
                    // The installed six-output profile used the same device UID
                    // and storage key. Preserve its validated assignments when
                    // upgrading; new outputs are unassigned until configured.
                    valid=false;
                    if constexpr(Channels==12) {
                        std::array<UInt32,6> previous{};
                        if(decode(storedLabels,previous)) {
                            typename HalSpeakerPreferences<6>::Layout old;
                            for(size_t i=0;i<6;++i)old.descriptions[i].mChannelLabel=previous[i];
                            valid=HalSpeakerPreferences<6>::validLayout(old) &&
                                HalSpeakerPreferences<6>::validStereo(restored.stereo);
                            if(valid)std::copy(previous.begin(),previous.end(),labels.begin());
                        }
                    }
                }
            }
            if(valid) {
                for(size_t i=0;i<Channels;++i)restored.layout.descriptions[i].mChannelLabel=labels[i];
                valid=validLayout(restored.layout) && validStereo(restored.stereo);
            }
            if(valid){std::lock_guard lock(mutex_);state_=restored;}
        }
        if(saved)CFRelease(saved);
        ready_.store(true,std::memory_order_release);return valid;
    }
    OSStatus get(AudioObjectPropertySelector selector,UInt32 capacity,UInt32 *used,void *out) const {
        if(!used)return kAudioHardwareIllegalOperationError;*used=0;
        const auto s=snapshot();const bool layout=selector==kAudioDevicePropertyPreferredChannelLayout;
        const UInt32 size=layout?sizeof(s.layout):sizeof(s.stereo);
        if(capacity<size || !out)return kAudioHardwareBadPropertySizeError;
        std::memcpy(out,layout?static_cast<const void *>(&s.layout):s.stereo.data(),size);*used=size;return noErr;
    }
    OSStatus set(AudioServerPlugInHostRef host,AudioObjectPropertySelector selector,UInt32 size,const void *data,bool &changed) {
        changed=false;
        if(!ready_.load(std::memory_order_acquire) || !host || !host->WriteToStorage)return kAudioHardwareIllegalOperationError;
        if(writer_.test_and_set(std::memory_order_acquire))return kAudioHardwareIllegalOperationError;
        WriteGuard guard{writer_};auto next=snapshot();
        if(selector==kAudioDevicePropertyPreferredChannelLayout) {
            const auto status=normalize(size,data,next.layout);if(status)return status;
        } else {
            if(size!=sizeof(next.stereo) || !data)return kAudioHardwareBadPropertySizeError;
            std::memcpy(next.stereo.data(),data,size);
            if(!validStereo(next.stereo))return kAudioHardwareIllegalOperationError;
        }
        const auto old=snapshot();
        if(std::memcmp(&old.layout,&next.layout,sizeof(Layout))==0 && old.stereo==next.stereo)return noErr;
        auto saved=encode(next);if(!saved)return kAudioHardwareUnspecifiedError;
        // No property-state lock across host calls: the host may reenter reads.
        // Concurrent/reentrant writers fail explicitly instead of deadlocking.
        const auto status=host->WriteToStorage(host,storageKey(),saved);CFRelease(saved);
        if(status)return status;
        {std::lock_guard lock(mutex_);state_=next;}
        changed=true;return noErr;
    }
};
}
