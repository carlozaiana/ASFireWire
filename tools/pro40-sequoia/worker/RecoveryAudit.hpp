// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include "../ColdClockAdmission.hpp"
namespace Pro40 {
// Read-only, original Pro40 low-rate layout. Cross-checked against Linux
// dice-interface.h:30-57,244-262,294-325 and Pro40ChannelConfig's validated parser.
// Backend binds reads to one generation and rechecks it with stable().
template<class IO> bool idleSampleForOwner(IO &io,uint64_t owner,bool coldInitialization=false) {
    constexpr uint64_t base=0xffffe0000000ULL;
    if(!io.stable())return false;
    std::array<uint64_t,3> offset{},size{};
    for(unsigned i=0;i<3;++i) {
        uint32_t o=0,s=0;
        if(!io.read(base+i*8,o) || !io.read(base+i*8+4,s))return false;
        offset[i]=uint64_t(o)*4;size[i]=uint64_t(s)*4;
        if(offset[i]<40 || size[i]<8 || offset[i]+size[i]>65536)return false;
        for(unsigned j=0;j<i;++j)
            if(offset[i]<offset[j]+size[j] && offset[j]<offset[i]+size[i])return false;
    }
    if(size[0]<0x60)return false;
    const auto global=base+offset[0];
    const std::array<uint32_t,6> field{0,4,0x4c,0x50,0x54,0x5c};
    const std::array<uint32_t,6> expected{uint32_t(owner>>32),uint32_t(owner),0x20c,0,0x201,48000};
    std::array<uint32_t,6> observed{};
    for(unsigned i=0;i<field.size();++i) {
        uint32_t value=0;
        if(!io.read(global+field[i],value))return false;
        observed[i]=value;
        if((!coldInitialization || (i!=2 && i!=4 && i!=5)) && value!=expected[i])return false;
    }
    if(coldInitialization && !cold48kClock(observed[2],observed[4],observed[5]))return false;
    for(unsigned direction=0;direction<2;++direction) {
        const auto address=base+offset[direction+1];uint32_t count=0,stride=0;
        if(!io.read(address,count) || !io.read(address+4,stride))return false;
        const uint64_t bytes=uint64_t(stride)*4;
        if(count!=2 || bytes<(direction?20u:16u) || 8+count*bytes>size[direction+1])return false;
        for(unsigned stream=0;stream<2;++stream) {
            const auto start=address+8+stream*bytes;
            uint32_t channel=0,pcm=0,midi=0;
            const unsigned fields=direction?8u:4u;
            if(!io.read(start,channel) || !io.read(start+fields,pcm) || !io.read(start+fields+4,midi))return false;
            if(channel!=0xffffffff || pcm!=(direction?(stream?8u:12u):10u) || midi!=(stream?0u:1u))return false;
        }
    }
    // Bracket the layout reads with owner/ENABLE rechecks as well as generation.
    for(unsigned i=0;i<field.size();++i) {
        uint32_t value=0;
        if(!io.read(global+field[i],value) || value!=observed[i])return false;
    }
    return io.stable();
}
template<class IO> bool recoveryIdleSample(IO &io) {
    return idleSampleForOwner(io,0xffff000000000000ULL);
}
template<class IO> bool coldRecoveryIdleSample(IO &io) {
    return idleSampleForOwner(io,0xffff000000000000ULL,true);
}
}
