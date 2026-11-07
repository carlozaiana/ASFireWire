// SPDX-License-Identifier: Apache-2.0
#include "DeviceLease.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
using namespace Pro40;
namespace fs=std::filesystem;
static std::optional<LeaseBootIdentity> bootA() {
    LeaseBootIdentity b{};std::memcpy(b.data(),"11111111-1111-1111-1111-111111111111",36);return b;
}
static std::optional<LeaseBootIdentity> bootB() {
    LeaseBootIdentity b{};std::memcpy(b.data(),"22222222-2222-2222-2222-222222222222",36);return b;
}
template<class Work> void childCheck(Work work) {
    const auto pid=fork();assert(pid>=0);
    if(pid==0){work();std::_Exit(0);}
    int status=0;assert(waitpid(pid,&status,0)==pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
}
int main() {
    char pattern[]="build/pro40-sequoia/lease-check-XXXXXX";
    const auto directory=mkdtemp(pattern);assert(directory);
    const fs::path root(directory);
    {
        DeviceLease owner;assert(owner.acquire(directory,1)==DeviceAdmission::acquired);
        childCheck([&] {
            DeviceLease competitor;assert(competitor.acquire(directory,1)==DeviceAdmission::busy);
            DeviceLease other;assert(other.acquire(directory,2)==DeviceAdmission::acquired);
            assert(other.confirmClean());
        });
        assert(owner.confirmClean());
        // Clean marker does not release the still-held kernel lock.
        childCheck([&] {DeviceLease competitor;assert(competitor.acquire(directory,1)==DeviceAdmission::busy);});
    }
    {DeviceLease next;assert(next.acquire(directory,1)==DeviceAdmission::acquired);assert(next.confirmClean());}
    childCheck([&] {DeviceLease abandoned;assert(abandoned.acquire(directory,3)==DeviceAdmission::acquired);std::_Exit(0);});
    for(unsigned i=0;i<3;++i) {
        DeviceLease blocked;assert(blocked.acquire(directory,3)==DeviceAdmission::unclean);
        assert(!blocked.confirmClean());
    }
    // Recovery cannot touch a live holder or audit a missing record.
    {
        DeviceLease owner;assert(owner.acquire(directory,8)==DeviceAdmission::acquired);
        childCheck([&] {DeviceLease recovery;assert(recovery.recover(directory,8,[]{assert(false);return true;})==DeviceRecovery::busy);});
        assert(owner.confirmClean());
    }
    {DeviceLease recovery;assert(recovery.recover(directory,99,[]{assert(false);return true;})==DeviceRecovery::ioError);}
    assert(!fs::exists(root/"0000000000000063.lock"));
    const auto abandoned=root/"0000000000000003.lock";
    struct stat before{},after{};assert(stat(abandoned.c_str(),&before)==0);
    {DeviceLease recovery;assert(recovery.recover(directory,3,[]{return false;})==DeviceRecovery::auditRejected);}
    {std::ifstream input(abandoned);std::string value;std::getline(input,value);assert(value=="active");}
    {
        DeviceLease recovery;
        assert(recovery.recover(directory,3,[&]{
            childCheck([&]{DeviceLease competitor;assert(competitor.acquire(directory,3)==DeviceAdmission::busy);});
            return true;
        })==DeviceRecovery::recovered);
        childCheck([&]{DeviceLease competitor;assert(competitor.acquire(directory,3)==DeviceAdmission::busy);});
    }
    assert(stat(abandoned.c_str(),&after)==0 && before.st_ino==after.st_ino);
    {DeviceLease recovery;assert(recovery.recover(directory,3,[]{return true;})==DeviceRecovery::alreadyClean);}
    {DeviceLease next;assert(next.acquire(directory,3)==DeviceAdmission::acquired);assert(next.confirmClean());}
    // Automatic recovery may inspect hardware only for an earlier, proven
    // boot. Same-boot faults and legacy/unbound writes remain blocked.
    {DeviceLease old(bootA);assert(old.acquire(directory,20)==DeviceAdmission::acquired);}
    {DeviceLease same(bootA);assert(same.recover(directory,20,[]{assert(false);return true;},true)==DeviceRecovery::bootNotProven);}
    {DeviceLease newer(bootB);assert(newer.recover(directory,20,[]{return false;},true)==DeviceRecovery::auditRejected);}
    {DeviceLease newer(bootB);assert(newer.recover(directory,20,[]{return true;},true)==DeviceRecovery::recovered);}
    {DeviceLease old(bootA);assert(old.acquire(directory,21)==DeviceAdmission::acquired);}
    const auto legacy=root/"0000000000000015.lock";
    {std::ofstream change(legacy);change<<"active\n";}
    {DeviceLease newer(bootB);assert(newer.recover(directory,21,[]{assert(false);return true;},true)==DeviceRecovery::bootNotProven);}
    {DeviceLease old(bootA);assert(old.acquire(directory,22)==DeviceAdmission::acquired);}
    const auto unknown=root/"0000000000000016.lock";
    assert(!removexattr(unknown.c_str(),"local.pro40.lease-boot",0));
    {DeviceLease newer(bootB);assert(newer.recover(directory,22,[]{assert(false);return true;},true)==DeviceRecovery::bootNotProven);}
    {std::ofstream file(root/"0000000000000004.lock");file<<"cle";}
    chmod((root/"0000000000000004.lock").c_str(),0600);
    {DeviceLease partial;assert(partial.acquire(directory,4)==DeviceAdmission::unclean);}
    fs::create_symlink(root/"0000000000000001.lock",root/"0000000000000005.lock");
    {DeviceLease link;assert(link.acquire(directory,5)==DeviceAdmission::ioError);}
    fs::create_hard_link(root/"0000000000000001.lock",root/"0000000000000006.lock");
    {DeviceLease link;assert(link.acquire(directory,6)==DeviceAdmission::ioError);}
    chmod(directory,0755);
    {DeviceLease unsafe;assert(unsafe.acquire(directory,7)==DeviceAdmission::ioError);}
    chmod(directory,0700);
    // Test-private directory only, after all workers and lease owners exited.
    fs::remove_all(root);
    std::puts("PASS: cross-process exclusion, distinct GUIDs, clean reuse, lock held after clean marker, persistent unknown-exit block, partial records and unsafe paths rejected");
}
