// SPDX-License-Identifier: Apache-2.0
// Executes real DmaPreparation release/callback code with fake native ports.
// No FireWire device is opened. PROT_NONE makes stale DMA reads fatal even when
// the metadata/refcon object remains alive, as in the hardware crash.
#define PRO40_DMA_TEARDOWN_TEST
#define PRO40_DMA_LIFECYCLE
#include "DmaPreparation.hpp"
#include <cassert>
#include <functional>
#include <memory>
#include <vector>
#include <sys/wait.h>

static std::vector<std::function<void()>> pending;
static size_t delivered=0;
static bool nativeClosed=false;
static unsigned reclaimedPools=0;

// Interpose only in this test executable. Every fake delivery runs the real
// production callback; no native library operation or real runloop is used.
extern "C" CFRunLoopRunResult CFRunLoopRunInMode(CFRunLoopMode,CFTimeInterval,Boolean) {
    if(delivered<pending.size())pending[delivered++]();
    return kCFRunLoopRunHandledSource;
}

struct DmaPreparationTest {
    IOFireWireLocalIsochPortInterface localTable{};
    IOFireWireLocalIsochPortInterface *local=&localTable;
    IOFireWireNuDCLPoolInterface poolTable{};
    IOFireWireNuDCLPoolInterface *pool=&poolTable;
    DmaPreparation port;
    explicit DmaPreparationTest(FILE *log,unsigned index,bool started=true) {
        localTable.Release=[](void *)->ULONG{return 2;}; // Deferred stop token.
        poolTable.Release=[](void *)->ULONG{assert(nativeClosed);++reclaimedPools;return 1;};
        port.log_=log;port.index_=index;port.local_=&local;port.pool_=&pool;
        port.needsFinalize_=started;port.callbackStorageExposed_=true;
        port.bufferBytes_=size_t(getpagesize())*2;
        port.buffer_=mmap(nullptr,port.bufferBytes_,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
        assert(port.buffer_!=MAP_FAILED);
        auto *words=static_cast<UInt32 *>(port.buffer_);
        for(unsigned i=0;i<8;++i) {
            auto &m=port.metadata_[i];m.owner=&port;m.index=i;
            m.timestamp=words+i*2;m.status=m.timestamp+1;
            *m.timestamp=0x1000+i*0x1000;*m.status=0x11;
        }
    }
    void completion(unsigned i){DmaPreparation::completed(&port.metadata_[i],nullptr);}
    void finish(){(void)DmaPreparation::finalized(&port);}
    void guardMapping(){assert(!mprotect(port.buffer_,port.bufferBytes_,PROT_NONE));}
    bool mapped()const{return port.buffer_ && port.pool_;}
    void queueNormal() {
        if(port.index_>=2)for(unsigned i=0;i<8;++i)pending.push_back([this,i]{completion(i);});
        pending.push_back([this]{finish();});
    }
};

static void reset() {pending.clear();delivered=0;nativeClosed=false;reclaimedPools=0;}
static void runFour(bool late) {
    reset();auto *log=tmpfile();assert(log);
    std::array<std::unique_ptr<DmaPreparationTest>,4> ports;
    for(unsigned i=0;i<4;++i)ports[i]=std::make_unique<DmaPreparationTest>(log,i);
    for(unsigned i=0;i<4;++i) {
        if(i==3 && late)pending.push_back([&]{ports[2]->completion(6);});
        ports[i]->queueNormal();
    }
    for(unsigned i=0;i<4;++i) {
        const auto r=ports[i]->port.releaseDetailed();assert(!r.native && !r.evidence);
        assert(ports[i]->mapped() && reclaimedPools==0);
        if(i==2 && late)ports[i]->guardMapping();
    }
    assert(delivered==pending.size());
    assert(ports[2]->port.callbacks()==8); // Stale completion never counts as input.
    assert(bool(ports[2]->port.callbackStatus())==late);
    nativeClosed=true;
    for(unsigned i=0;i<4;++i) {
        assert(bool(ports[i]->port.reclaimAfterNativeClose())==(late && i==2));
        assert(!ports[i]->mapped());
    }
    assert(reclaimedPools==4);
    fclose(log);ports={}; // No callback/log use on a second destructor release.
}

static void retainedAcrossAttempt() {
    reset();auto *log=tmpfile();assert(log);
    auto old=std::make_unique<DmaPreparationTest>(log,2);
    old->queueNormal();assert(!old->port.releaseDetailed().native);
    old->guardMapping();
    auto current=std::make_unique<DmaPreparationTest>(log,3,false);
    // A failed channel Start can still require the deferred finalize callback.
    pending.push_back([&]{old->completion(6);});current->queueNormal();
    assert(!current->port.releaseDetailed().native);
    assert(old->port.callbackStatus() && !current->port.callbackStatus());
    assert(reclaimedPools==0);nativeClosed=true;
    assert(old->port.reclaimAfterNativeClose()==kIOReturnError);
    assert(!current->port.reclaimAfterNativeClose());
    fclose(log);
}

static void refusesUnsafeRetirement(bool missingFinalize) {
    const auto child=fork();assert(child>=0);
    if(child==0) {
        reset();auto *log=tmpfile();assert(log);
        {DmaPreparationTest pendingPort(log,0);
            assert(pendingPort.port.reclaimAfterNativeClose()==kIOReturnBusy);
            if(missingFinalize)(void)pendingPort.port.releaseDetailed();
        }
        _exit(99);
    }
    int result=0;assert(waitpid(child,&result,0)==child);
    assert(WIFEXITED(result) && WEXITSTATUS(result)==3);
}

int main() {
    runFour(false);runFour(true);retainedAcrossAttempt();
    refusesUnsafeRetirement(false);refusesUnsafeRetirement(true);
    puts("PASS: normal drain, late sibling/old-attempt callbacks, protected mappings, partial-start finalization, timeout and unsafe-unwind refusal");
}
