// SPDX-License-Identifier: Apache-2.0
#include "DeviceLease.hpp"
#include "DeviceLeasePath.hpp"
#include "RecoveryAudit.hpp"
#include "../NativeFireWire.h"
#include <charconv>
#include <chrono>
#include <thread>
namespace {
struct ReadOnlyAudit {
    uint32_t generation;uint16_t node;FILE *log;
    bool stable() {
        uint32_t g=0;uint16_t n=0;const auto status=pro40_state(&g,&n);
        std::fprintf(log,"audit-generation generation=%u node=%u status=%08x\n",g,n,unsigned(status));
        return !status && g==generation && n==node;
    }
    bool read(uint64_t address,uint32_t &value) {
        std::array<uint8_t,4> bytes{};
        const auto status=pro40_read(generation,address,4,bytes.data());
        value=uint32_t(bytes[0])<<24|uint32_t(bytes[1])<<16|uint32_t(bytes[2])<<8|bytes[3];
        std::fprintf(log,"audit-read address=%012llx value=%08x status=%08x\n",
            static_cast<unsigned long long>(address),value,unsigned(status));
        return !status;
    }
};
}
int main(int argc,char **argv) {
    if((argc!=5 && argc!=6) || std::string(argv[1])!="--guid" || std::string(argv[3])!="--log" ||
       (argc==6 && std::string(argv[5])!="--after-reboot"))return 64;
    const std::string input=argv[2];uint64_t guid=0;
    const auto parsed=std::from_chars(input.data(),input.data()+input.size(),guid,16);
    if(parsed.ec!=std::errc{} || parsed.ptr!=input.data()+input.size())return 64;
    FILE *log=std::fopen(argv[4],"wx");if(!log)return 1;
    const auto directory=Pro40::deviceLeaseDirectory();
    if(!directory){std::fclose(log);return 1;}
    Pro40::DeviceLease lease;
    const auto result=lease.recover(directory->c_str(),guid,[&] {
        auto status=pro40_open(guid);
        std::fprintf(log,"audit-open status=%08x\n",unsigned(status));
        uint32_t generation=0;uint16_t node=0;
        if(!status)status=pro40_state(&generation,&node);
        bool ok=!status;
        ReadOnlyAudit io{generation,node,log};
        for(unsigned sample=0;ok && sample<2;++sample) {
            if(sample)std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ok=Pro40::coldRecoveryIdleSample(io);
            std::fprintf(log,"audit-sample index=%u idle=%u\n",sample,unsigned(ok));
        }
        pro40_close();
        // Preserve the audit before authorizing the record write.
        return ok && std::fflush(log)==0 && fsync(fileno(log))==0;
    },argc==6);
    const char *name=result==Pro40::DeviceRecovery::recovered?"recovered":
        result==Pro40::DeviceRecovery::alreadyClean?"already-clean":
        result==Pro40::DeviceRecovery::busy?"busy":
        result==Pro40::DeviceRecovery::auditRejected?"audit-rejected":
        result==Pro40::DeviceRecovery::bootNotProven?"boot-not-proven":"io-error";
    std::fprintf(log,"recovery-result=%s\n",name);
    std::printf("Recovery: %s. Hardware was read only. Audit: %s\n",name,argv[4]);
    if(result==Pro40::DeviceRecovery::busy)
        std::fprintf(stderr,"Another worker holds the device lock; recovery did not access the device.\n");
    else if(result==Pro40::DeviceRecovery::auditRejected)
        std::fprintf(stderr,"Idle baseline was not verified. The block remains; inspect the audit before retrying.\n");
    else if(result==Pro40::DeviceRecovery::ioError)
        std::fprintf(stderr,"Cannot safely access or update the existing record. Do not delete the lock file.\n");
    const bool closed=std::fclose(log)==0;
    return closed && (result==Pro40::DeviceRecovery::recovered || result==Pro40::DeviceRecovery::alreadyClean)?0:2;
}
