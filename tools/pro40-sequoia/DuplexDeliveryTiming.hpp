// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstdint>
namespace Pro40Audio {
// Dedicated delivery owner only; inspect after join. No allocation, atomics,
// logging or clock reads here. Caller supplies Mach ticks at loop boundaries.
struct DuplexDeliveryTiming {
    uint64_t iterations=0,lastBegin=0,lastEnd=0,maxGap=0,maxWork=0;
    uint64_t maxGapBegin=0,maxGapEnd=0;
    void begin(uint64_t now) noexcept {
        if(lastEnd && now>=lastEnd && now-lastEnd>maxGap) {
            maxGap=now-lastEnd;maxGapBegin=lastEnd;maxGapEnd=now;
        }
        lastBegin=now;++iterations;
    }
    void end(uint64_t now) noexcept {
        if(now>=lastBegin)maxWork=std::max(maxWork,now-lastBegin);
        lastEnd=now;
    }
};
}
