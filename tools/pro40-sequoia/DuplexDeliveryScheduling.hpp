// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <time.h>
#include <algorithm>
#include <cstdint>
namespace Pro40Audio {
// One dedicated, single-session delivery thread. Enter after negotiation and
// allocation; restore before logging/closing, then exit. Never use on a HAL
// callback or borrowed dispatch thread. This deliberately avoids CoreAudio
// workgroup RPC during StartIO (see NativeAudioScheduling's cold-start check).
// SDK mach/thread_policy.h:115-166 specifies absolute ticks and the budget
// relation. Existing NativeAudioScheduling validates policy readback/restore
// on Sequoia. These are scheduling requests, not guarantees: keep all queue,
// clock-age, continuity, teardown and underrun guards.
class DuplexDeliveryScheduling {
public:
    static constexpr uint64_t periodNS=2000000,computationNS=1000000,constraintNS=2000000;
    struct Evidence {
        int before=-1,set=-1,after=-1,finalRead=-1,restore=-1,restoredRead=-1;
        uint64_t intervals=0,overruns=0,maxCpuNS=0,maxWorkNS=0;
        bool entered=false,closed=false;
    } evidence;
private:
    mach_timebase_info_data_t tb_{};
    thread_time_constraint_policy_data_t requested_{};
    const pthread_t owner_=pthread_self();
    bool attempted_=false,active_=false,interval_=false,closedClean_=false;
    uint64_t begin_=0,cpuBegin_=0;
    static int read(thread_time_constraint_policy_data_t &policy,bool &isDefault) noexcept {
        mach_msg_type_number_t count=THREAD_TIME_CONSTRAINT_POLICY_COUNT;boolean_t value=false;
        const int r=thread_policy_get(pthread_mach_thread_np(pthread_self()),THREAD_TIME_CONSTRAINT_POLICY,
            reinterpret_cast<thread_policy_t>(&policy),&count,&value);
        isDefault=value;return r?r:count==THREAD_TIME_CONSTRAINT_POLICY_COUNT?0:KERN_FAILURE;
    }
    bool matches(const thread_time_constraint_policy_data_t &p)const noexcept {
        return p.period==requested_.period && p.computation==requested_.computation && p.constraint==requested_.constraint;
    }
public:
    DuplexDeliveryScheduling()=default;
    DuplexDeliveryScheduling(const DuplexDeliveryScheduling &)=delete;
    bool active()const noexcept{return active_;}
    [[nodiscard]] bool enter()noexcept {
        if(!pthread_equal(owner_,pthread_self()) || attempted_ || evidence.closed)return false;
        attempted_=true;
        if(mach_timebase_info(&tb_) || !tb_.numer || !tb_.denom)return false;
        const auto ticks=[&](uint64_t ns){return __uint128_t(ns)*tb_.denom/tb_.numer;};
        if(!ticks(computationNS) || ticks(periodNS)>UINT32_MAX)return false;
        requested_={uint32_t(ticks(periodNS)),uint32_t(ticks(computationNS)),uint32_t(ticks(constraintNS)),true};
        thread_time_constraint_policy_data_t before{},after{};bool isDefault=false;
        evidence.before=read(before,isDefault);
        if(evidence.before || !isDefault)return false; // Do not steal another owner's reservation.
        evidence.set=thread_policy_set(pthread_mach_thread_np(pthread_self()),THREAD_TIME_CONSTRAINT_POLICY,
            reinterpret_cast<thread_policy_t>(&requested_),THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        if(evidence.set)return false;
        active_=true;evidence.entered=true;
        evidence.after=read(after,isDefault);
        return !evidence.after && !isDefault && matches(after);
    }
    void begin()noexcept {
        if(!active_ || interval_)return;
        begin_=mach_absolute_time();cpuBegin_=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);interval_=true;
    }
    void end()noexcept {
        if(!interval_)return;
        const auto now=mach_absolute_time(),cpu=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
        const auto wall=uint64_t(__uint128_t(now-begin_)*tb_.numer/tb_.denom);
        evidence.maxWorkNS=std::max(evidence.maxWorkNS,wall);
        evidence.maxCpuNS=std::max(evidence.maxCpuNS,cpu-cpuBegin_);
        evidence.overruns+=wall>constraintNS || cpu-cpuBegin_>computationNS;
        ++evidence.intervals;interval_=false;
    }
    [[nodiscard]] bool wait()noexcept {
        if(!active_)return false;
        end();
        const auto now=mach_absolute_time();
        // No catch-up spin after an overrun. A fresh period bounds work bursts;
        // steady state wakes on the original period, not work+relative sleep.
        const auto deadline=begin_ && now>=begin_ && now-begin_<requested_.period?
            begin_+requested_.period:now+requested_.period;
        const auto result=mach_wait_until(deadline);
        // A signal can interrupt the wait; let the owner's stop/fault checks
        // run instead of converting an ordinary cancellation into an xrun.
        return result==KERN_SUCCESS || result==KERN_ABORTED;
    }
    [[nodiscard]] bool close()noexcept {
        if(!pthread_equal(owner_,pthread_self()))return false;
        if(evidence.closed)return closedClean_;
        end();closedClean_=true;
        if(active_) {
            thread_time_constraint_policy_data_t final{},restored{};bool isDefault=false;
            evidence.finalRead=read(final,isDefault);
            const bool retained=!evidence.finalRead && !isDefault && matches(final);
            evidence.restore=thread_policy_set(pthread_mach_thread_np(pthread_self()),THREAD_STANDARD_POLICY,nullptr,THREAD_STANDARD_POLICY_COUNT);
            evidence.restoredRead=read(restored,isDefault);
            closedClean_=retained && !evidence.restore && !evidence.restoredRead && isDefault;
            active_=false;
        }
        evidence.closed=true;return closedClean_;
    }
    ~DuplexDeliveryScheduling(){(void)close();}
};
}
