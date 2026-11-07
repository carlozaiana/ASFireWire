// SPDX-License-Identifier: Apache-2.0
#include "RecoveryAudit.hpp"
#include <cassert>
#include <cstdio>
#include <map>
struct Fixture {
    static constexpr uint64_t base=0xffffe0000000ULL;
    std::map<uint64_t,uint32_t> words;
    unsigned checks=0;
    unsigned failCheck=0;
    uint64_t failRead=0;
    bool stable(){return ++checks!=failCheck;}
    bool read(uint64_t address,uint32_t &value) {
        if(address==failRead || !words.contains(address))return false;
        value=words.at(address);return true;
    }
    Fixture() {
        words={{base,16},{base+4,32},{base+8,48},{base+12,16},
               {base+16,64},{base+20,16}};
        const uint64_t global=base+64;
        words[global]=0xffff0000;words[global+4]=0;
        words[global+0x4c]=0x20c;words[global+0x50]=0;
        words[global+0x54]=0x201;words[global+0x5c]=48000;
        for(unsigned d=0;d<2;++d) {
            const auto section=base+(d?256:192);
            words[section]=2;words[section+4]=d?5:4;
            for(unsigned s=0;s<2;++s) {
                const auto start=section+8+s*(d?20:16);
                words[start]=0xffffffff;
                words[start+(d?8:4)]=d?(s?8:12):10;
                words[start+(d?12:8)]=s?0:1;
            }
        }
    }
};
int main() {
    Fixture owned;owned.words[Fixture::base+64]=0xffc10001;owned.words[Fixture::base+68]=0x100;
    assert(Pro40::idleSampleForOwner(owned,0xffc1000100000100ULL));
    assert(!Pro40::recoveryIdleSample(owned));
    assert(!Pro40::idleSampleForOwner(owned,0xffc1000100000101ULL));
    Fixture valid;assert(Pro40::recoveryIdleSample(valid));assert(valid.checks==2);
    for(uint32_t selected:{0x10cu,0x20cu}) {
        Fixture cold;
        cold.words[Fixture::base+64+0x4c]=selected;
        cold.words[Fixture::base+64+0x54]=0x101;
        cold.words[Fixture::base+64+0x5c]=44100;
        assert(!Pro40::recoveryIdleSample(cold));
        assert(Pro40::coldRecoveryIdleSample(cold));
        cold.words[Fixture::base+64+0x50]=1;
        assert(!Pro40::coldRecoveryIdleSample(cold));
    }
    for(auto [status,rate]:{std::pair{0x100u,44100u},std::pair{0x201u,44100u},
                           std::pair{0x101u,48000u},std::pair{0x401u,96000u}}) {
        Fixture cold;cold.words[Fixture::base+64+0x54]=status;cold.words[Fixture::base+64+0x5c]=rate;
        assert(!Pro40::coldRecoveryIdleSample(cold));
    }
    Fixture external;external.words[Fixture::base+64+0x4c]=0x201;
    assert(!Pro40::coldRecoveryIdleSample(external));
    Fixture channels;channels.words[Fixture::base+200]=1;
    assert(!Pro40::coldRecoveryIdleSample(channels));
    // Every required read must succeed; every field has an enforced value/bound.
    for(const auto &[address,value]:Fixture{}.words) {
        (void)value;
        Fixture failed;failed.failRead=address;assert(!Pro40::recoveryIdleSample(failed));
    }
    for(unsigned checkpoint:{1u,2u}) {
        Fixture changed;changed.failCheck=checkpoint;assert(!Pro40::recoveryIdleSample(changed));
    }
    const auto base=Fixture::base;
    for(const auto &[address,value]:std::map<uint64_t,uint32_t>{
        {base,0xffffffff},{base+4,0xffffffff},{base+8,16},
        {base+64,0},{base+68,1},{base+64+0x50,1},
        {base+64+0x4c,0x10c},{base+64+0x54,0},{base+64+0x5c,44100},
        {base+192,3},{base+196,0xffffffff},{base+200,1},
        {base+204,8},{base+208,0},{base+256+8+8,10}}) {
        Fixture changed;changed.words[address]=value;assert(!Pro40::recoveryIdleSample(changed));
    }
    std::puts("PASS: idle recovery audit, all read failures, generation changes, layout/owner/enable/clock/channel/geometry refusal");
}
