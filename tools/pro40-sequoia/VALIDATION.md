# Validation through 2026-11-07

Results apply to one original Saffire Pro 40 and one M1 Mac Studio running
Sequoia 15.7.9. User observations, controlled hardware runs and software checks
are distinguished below. Private captures and machine identifiers are omitted.

## Operational feedback

On **7 November 2026**, the owner reported: “the interface is very solid now and
working flawlessly since the last change.” The last installed driver change was
the **19 October** startup update. This supports reported reliable normal use
over that interval; it is not a measured continuous uptime, an uninterrupted
19-day test, or a controlled three-hour Nuendo qualification.

The latest saved automatic login warm-up completed its finite stream and joined
cleanup with transfer/evidence/cleanup all zero. Its audit reported already-clean.
The installed keeper and native stream were running during the November read-only
inspection. This establishes a successful automatic login with a clean lease,
not a hardware test of automatic recovery from a stamped stale active lease.

## Hardware/application evidence

| Check | Observation and limit |
| --- | --- |
| CoreAudio/Nuendo ports | 8 analog inputs and 12 selectable playback outputs at internal 48 kHz; all twelve listed in Nuendo and Audio MIDI Setup |
| Speaker preferences | Audio MIDI Setup accepted assignments without the overlap error; saved layout/stereo preferences survived restart; an Output 12 assignment read back correctly and was restored |
| Physical speaker mapping | L, R, C, LFE, LS, RS on connected outputs 1–6; physical outputs 7–12 were not listening-tested |
| Nuendo recording | Input 1 microphone take replayed cleanly through L/R; direct monitoring used |
| Shared audio | Owner observed simultaneous Nuendo and browser playback |
| QuickTime MOV | All six speakers correct; audio/picture appeared synchronized, without an instrumented offset or drift measurement |
| QuickTime WAV | Original rear-surround-labelled WAV lost surrounds in the tested Sequoia path; VLC played all six; changing only the mask in a comparison copy restored QuickTime playback |
| Earlier restart checks | 12/12 short native starts passed; joined shutdown; first-payload malformed signature reproduced offline, not observed in those starts |
| Delivery fix, 15 October | 180 s, 24 s and 600 s silent sessions passed; the ten-minute run included 60 s of four-thread utility CPU load |
| Ten-minute delivery margins | Native queue minimum 5,608 frames; maximum native wait 2.219 ms/work 0.519 ms; HAL wait 2.087 ms/work 0.233 ms; zero scheduling-budget overruns; one full Mach send retried without dropping PCM |
| Startup fix, 19 October | Idle actual 44.1 kHz admitted, explicitly initialized to 48 kHz; finite silent warm-up and joined cleanup passed |
| Updated installed sessions | 180 s and fresh 24 s silent CoreAudio runs passed input presence, transport, evidence and cleanup checks; zero reported scheduling overruns |
| Persistent stream | Keeper restored; warm client opened in 15 ms with the same native worker; default Pro40 output restored |
| Physical meters | Owner confirmed responsive after the 19 October sessions, and FW Active on afterward; continuous observation during those two sessions was not claimed |

The clock-recovery hardware run used explicit recovery of an older, unstamped
lease after the user confirmed a host/interface power cycle. It did not execute
the new automatic previous-boot-provenance path. Same-boot and ambiguous cleanup
remain blocked. Earlier failures are retained as failures in the local evidence.

## Hardware-free verification

- ASan/UBSan and TSan: speaker preferences, concurrency, validation and persistence;
  twelve-channel PCM transport; startup/queue ownership; delivery-policy admission,
  readback and restoration on disposable threads.
- ASan/UBSan: cross-process lease exclusion, clean reuse, same-boot/unknown-provenance
  refusal, previous-boot acceptance only after successful audit, legacy rewrite
  rejection, idle clock/layout admission, and real teardown code with fake ports.
- Python: once-per-boot orchestration, settling/cancellation, audit ordering and
  refusal before native entry, missing-log admission diagnosis, supervision and
  explicit reuse. Hardware-free child fixtures are used.
- Packet/clock checks: pre-roll exclusion, strict subsequent packets, exact samples,
  replay/clock continuity, input handoffs, unavailable history and teardown.
- Generated-worker sanitizer builds and static native package/HAL builds inspect
  imports, exact peer identities and discovery properties without installing them.

Reproduction commands are in README. Fresh successful export checks from
7 November are recorded in [the November update](UPDATES_2026-11-07.md#clean-export-checks).

## Remaining limits

No production reliability guarantee or hardware/OS generalization is implied.
Physical round-trip latency is about 225.8 ms from an earlier loopback result;
warm opening times are different measurements. The cold-input workaround remains.
Three-hour endurance, systematic sleep/wake/hot unplug, automatic stale-active
boot recovery, portable installation, other rates/clocks, digital destinations
and MIDI require separate qualification. Nuendo project use does not substitute
for those controlled tests.
