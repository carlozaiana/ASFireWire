// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <optional>
#include <sys/sysctl.h>
namespace Pro40 {
using LeaseBootIdentity=std::array<char,37>;
inline bool validLeaseBoot(const LeaseBootIdentity &boot) {
    if(boot[36])return false;
    for(unsigned i=0;i<36;++i) {
        const char c=boot[i];
        if(i==8 || i==13 || i==18 || i==23) {if(c!='-')return false;}
        else if(!((c>='0' && c<='9') || (c>='A' && c<='F')))return false;
    }
    return true;
}
inline std::optional<LeaseBootIdentity> currentLeaseBoot() {
    LeaseBootIdentity boot{};size_t size=boot.size();
    if(sysctlbyname("kern.bootsessionuuid",boot.data(),&size,nullptr,0) ||
       size!=boot.size() || !validLeaseBoot(boot))return std::nullopt;
    return boot;
}
}
