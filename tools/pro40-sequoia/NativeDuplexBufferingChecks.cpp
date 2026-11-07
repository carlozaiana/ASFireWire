// SPDX-License-Identifier: Apache-2.0
#include "DuplexAudioFormat.hpp"
#include "NativeDuplexPolicy.hpp"
#include "PlaybackStartupGate.hpp"
#include <cassert>
#include <cstdio>
#include <memory>
using namespace Pro40Audio;
namespace {
constexpr unsigned batchFrames = 1536; // 256 FireWire cycles at48kHz.
int32_t sample(uint64_t frame, unsigned channel) {
    return int32_t((frame % 8000) * 13 + channel) - 52000;
}
struct Result {
    PlaybackStartupFault fault;
    uint64_t firstApplicationAt, firstApplicationFrame, lowWater, highWater;
};
// Virtual time uses device frames. Exercise the real two playback queues and
// startup gate with137-frame callbacks and bounded512-frame IPC transfers.
// Phase, admission and delayed wakes are varied independently. This models
// output availability only; actual process checks cover input/clock/lifecycle.
Result simulate(unsigned lead, unsigned prime, unsigned phase, unsigned admission,
                unsigned callbackDelay, unsigned deliveryDelay, unsigned seconds,
                bool stopProducing = false, unsigned nativeDelay = 0,
                unsigned callbackPhase = 0, unsigned nativePhase = 0) {
    auto host = std::make_unique<PlaybackPcmQueue<8192,duplexOutputChannels>>(19);
    auto native = std::make_unique<PlaybackPcmQueue<8192,duplexOutputChannels>>(19);
    BasicPlaybackStartupGate<duplexOutputChannels> gate(*native);
    assert(gate.schedule(lead, prime));
    std::array<int32_t, 4096 * duplexOutputChannels> zeros{};
    std::array<int32_t, 137 * duplexOutputChannels> callback{};
    std::array<int32_t, 512 * duplexOutputChannels> transfer{};
    std::array<int32_t, 8 * duplexOutputChannels> packet{};
    uint64_t callbackNumber = 0, nextDelivery = admission + deliveryDelay;
    uint64_t firstApplicationAt = UINT64_MAX;
    uint64_t firstApplicationFrame = UINT64_MAX;
    unsigned nativeBatch = 0;
    bool admitted = false, armed = false;
    for (uint64_t time = 0; time < uint64_t(seconds) * 48000; ++time) {
        if (time == admission) {
            assert(host->write(19, 0, std::span(zeros).first(prime * duplexOutputChannels)) == PlaybackQueueResult::ok);
            admitted = true;
        }
        if (admitted && !(stopProducing && time > 48000)) {
            // A late callback catches up in order; it does not skip samples.
            while (admission + callbackNumber * 137 + (callbackNumber % 23 == callbackPhase ? callbackDelay : 0) <= time) {
                const auto first = host->written();
                for (unsigned f = 0; f < 137; ++f) for (unsigned c = 0; c < duplexOutputChannels; ++c)
                    callback[f * duplexOutputChannels + c] = sample(first + f, c);
                assert(host->write(19, first, callback) == PlaybackQueueResult::ok);
                ++callbackNumber;
            }
        }
        if (admitted && time >= nextDelivery) {
            for (unsigned n = 0; n < 8; ++n) {
                const auto count = std::min<uint64_t>({512, host->written() - host->consumed(), native->writable()}) / 8 * 8;
                if (!count) break;
                auto bytes = std::span(transfer).first(count * duplexOutputChannels);
                assert(host->read(host->consumed(), bytes));
                assert(native->write(19, host->consumed(), bytes) == PlaybackQueueResult::ok);
                assert(host->commit(host->consumed(), count));
            }
            nextDelivery = time + std::max(1u, deliveryDelay);
            if (!armed && native->written() >= prime) { assert(gate.arm()); armed = true; }
        }
        // A delayed native wake can consume the accumulated burst at once;
        // absolute batch deadlines prevent the simulation hiding lost time.
        const auto delayed = nativeBatch % 17 == nativePhase ? nativeDelay : 0;
        if (time < phase + uint64_t(nativeBatch) * batchFrames + delayed) continue;
        ++nativeBatch;
        for (unsigned f = 0; f < batchFrames; f += 8) {
            const auto first = gate.nextNativeFrame(), consumed = native->consumed();
            packet.fill(0x123456);
            if (!gate.read(first, packet)) {
                // Failure neither advances nor fabricates a silent packet.
                assert(gate.nextNativeFrame() == first && native->consumed() == consumed);
                for (auto v : packet) assert(v == 0x123456);
                return {gate.fault(), firstApplicationAt, firstApplicationFrame, native->lowWater(), native->highWater()};
            }
            for (unsigned n = 0; n < 8; ++n) for (unsigned c = 0; c < duplexOutputChannels; ++c) {
                const bool application = first + n >= lead + prime;
                assert(packet[n * duplexOutputChannels + c] == (application ? sample(first + n - lead, c) : 0));
                if (application && firstApplicationAt == UINT64_MAX) {
                    firstApplicationAt = time;firstApplicationFrame = first + n;
                }
            }
            assert(gate.commit(first, 8));
        }
    }
    assert(gate.startedAt() == lead && firstApplicationAt != UINT64_MAX);
    assert(firstApplicationFrame == lead + prime);
    return {gate.fault(), firstApplicationAt, firstApplicationFrame, native->lowWater(), native->highWater()};
}
}
int main() {
    unsigned scenarios = 0;
    uint64_t low = UINT64_MAX, high = 0;
    for (unsigned phase : {0u, 137u, 768u, 1535u})
        for (unsigned admission : {0u, 960u, 1920u})
            for (unsigned callbackDelay : {0u, 480u, 960u})
                for (unsigned deliveryDelay : {0u, 480u})
                  for (unsigned nativeDelay : {0u, 960u})
                   for (const auto shifts : {std::pair{0u,0u},std::pair{7u,5u},std::pair{22u,16u}}) {
                    const auto r = simulate(nativeDuplexLeadFrames, nativeDuplexPrimeFrames,
                        phase, admission, callbackDelay, deliveryDelay, 2, false, nativeDelay, shifts.first, shifts.second);
                    assert(r.fault == PlaybackStartupFault::none);
                    low = std::min(low, r.lowWater); high = std::max(high, r.highWater); ++scenarios;
                }
    const auto longRun = simulate(nativeDuplexLeadFrames, nativeDuplexPrimeFrames, 0, 1920, 960, 480, 90, false, 960, 22, 16);
    assert(longRun.fault == PlaybackStartupFault::none);
    // Compare exact presentation-frame displacement. Wall-time preparation is
    // quantized into1536-frame banks and is NOT the physical playback time.
    const auto old = simulate(4096, 2048, 137, 960, 0, 0, 4);
    const auto reduced = simulate(nativeDuplexLeadFrames, nativeDuplexPrimeFrames, 137, 960, 0, 0, 4);
    assert(old.firstApplicationFrame - reduced.firstApplicationFrame == 1024); //21.333ms.
    assert(old.firstApplicationAt >= reduced.firstApplicationAt);
    unsigned tooSmallFailures=0;
    for(unsigned phase:{0u,137u,768u,1535u}) {
        const auto r=simulate(nativeDuplexLeadFrames,512,phase,1920,960,480,4);
        if(r.fault==PlaybackStartupFault::source)++tooSmallFailures;
    }
    assert(tooSmallFailures); // Do not choose a prime from happy-path pacing alone.
    const auto late = simulate(nativeDuplexLeadFrames, nativeDuplexPrimeFrames, 0, 4800, 0, 0, 4);
    assert(late.fault == PlaybackStartupFault::unprimed);
    const auto starved = simulate(nativeDuplexLeadFrames, nativeDuplexPrimeFrames, 0, 1920, 960, 480, 4, true);
    assert(starved.fault == PlaybackStartupFault::source);
    std::printf("PASS reduced prime: %u independently delayed timing combinations plus90s; exact12-channel PCM and priming, low/high=%llu/%llu frames;1024-frame earlier presentation;512-frame prime, late arm and underrun reject without fabricated samples\n",
        scenarios, (unsigned long long)low, (unsigned long long)high);
}
