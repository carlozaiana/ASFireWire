// SPDX-License-Identifier: Apache-2.0
#include "DuplexDeliveryScheduling.hpp"
#include "DuplexDeliveryTiming.hpp"
#include "AudioThreadScheduling.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
using namespace Pro40Audio;
int main() {
    DuplexDeliveryTiming timing;
    timing.begin(100);timing.end(150);timing.begin(200);timing.end(230);
    timing.begin(500);timing.end(600);
    assert(timing.iterations==3 && timing.maxWork==100 && timing.maxGap==270 &&
           timing.maxGapBegin==230 && timing.maxGapEnd==500);
    // Two disposable owners, as on each side of the real bridge. No hardware,
    // callbacks or borrowed dispatch threads; verify actual macOS policy.
    const auto run=[] {
        assert(AudioThreadScheduling::request().accepted());
        DuplexDeliveryScheduling scheduling;
        assert(scheduling.enter() && scheduling.active() && !scheduling.enter());
        for(unsigned n=0;n<100;++n){scheduling.begin();assert(scheduling.wait());}
        assert(scheduling.close() && scheduling.close() && !scheduling.active());
        const auto &e=scheduling.evidence;
        assert(e.entered && e.closed && e.intervals==100 && e.set==0 && e.after==0 &&
               e.finalRead==0 && e.restore==0 && e.restoredRead==0);
        std::printf("PASS delivery owner: periods=%llu maxCPU=%llu ns maxWork=%llu ns overruns=%llu; policy restored\n",
            (unsigned long long)e.intervals,(unsigned long long)e.maxCpuNS,
            (unsigned long long)e.maxWorkNS,(unsigned long long)e.overruns);
        assert(!scheduling.enter());
    };
    std::thread first(run),second(run);first.join();second.join();
    // No policy was changed when negotiation never reached streaming.
    DuplexDeliveryScheduling unused;assert(unused.close() && !unused.enter());
}
