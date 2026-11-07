# Experimental Saffire Pro 40 on macOS Sequoia

**Separate research implementation — work in progress, not a supported driver
release or a portable installer.** This native Sequoia/CoreAudio path depends on
Apple's IOFireWireFamily/IOFireWireLib/NuDCL stack. It does not replace ASFW's
DriverKit/OHCI implementation or work around the removal of that stack in Tahoe.

ASFW's maintainer closed [PR #199](https://github.com/mrmidi/ASFireWire/pull/199)
on 20 October 2026 because this approach is outside ASFW's scope. Development
material is retained on this fork branch as a separate experiment; it is not an
upstream-supported ASFW feature or an accepted contribution.

## Current state — 7 November 2026

One original DICE II/TCD2220 Saffire Pro 40 is in practical use with an M1 Mac
Studio on macOS Sequoia 15.7.9. On 7 November the owner reported that it had been
working flawlessly since the 19 October startup update. This is one-host user
feedback over that calendar interval, not a continuous 19-day endurance test.
Controlled tests and their limits are recorded in [VALIDATION.md](VALIDATION.md).

The device exposes **8 analog inputs and 12 selectable playback channels at
internal 48 kHz**. It is a normal CoreAudio device shared by Nuendo, browser
audio, VLC and other compatible apps. Outputs 1–6 initially describe L, R, C,
LFE, LS, RS; 7–12 start unassigned. Twelve playback channels do not mean twelve
analog connectors: the Pro 40 router/MixControl determines their physical
destinations. Listening tests covered the six connected speakers only.

Recent changes include persistent Audio MIDI Setup speaker assignments,
twelve-channel formats/IPC, bounded delivery-thread scheduling, explicit 48 kHz
initialization after use at 44.1 kHz, and guarded previous-boot lease recovery.
See [the November update](UPDATES_2026-11-07.md) for implementation and evidence.

QuickTime played the tested picture-cue MOV through all six speakers with
subjectively aligned audio/picture. A WAV using rear-surround labels still lost
its surround channels in the tested Sequoia QuickTime path; VLC played it, and a
comparison WAV with side-surround labels played correctly in QuickTime. This is
a documented mapping limitation, not universal QuickTime 5.1 qualification.

The tested original interface reports model `0x000005`, firmware 1.0.12. The host
uses Apple's Thunderbolt 3-to-2 and Thunderbolt-to-FireWire adapters. Other Pro 40
revisions and hosts have not been qualified. Development used AI assistance;
the interface owner performed the reported listening and physical observations.

## Architecture and review map

| Area | Entry points | Responsibility |
| --- | --- | --- |
| Native lifetime | `NativeFireWire.cpp`, `NativeSessionRunner.cpp`, `DmaPreparation.hpp` | Native ownership, descriptor reuse, generation checks and joined teardown |
| Packet content | `Pro40Packets.hpp`, `Pro40Replay.hpp`, `Pro40CaptureStartup.hpp` | CIP/AM824, timing replay and initial-payload quarantine |
| Input and clock | `SustainedInputHandoff.hpp`, `SustainedInputReceiver.hpp`, `SustainedClockPublication.hpp` | Copied samples, bounded history and device-derived timing |
| Playback/HAL | `DuplexAudioFormat.hpp`, `NativeDuplexBridge.hpp`, `HalSilenceDriver.cpp`, `HalSpeakerLayout.hpp` | 8/12 geometry, CoreAudio callbacks and stored speaker preferences |
| Delivery scheduling | `DuplexDeliveryScheduling.hpp`, `DuplexDeliveryTiming.hpp` | Dedicated-owner policy, absolute waits and retained fault evidence |
| Process ownership | `worker/`, `controller-lab/` | Exact development-signed identities, helper delegation and independent release receipts |
| Startup/session | `login-warmup/`, `worker/DeviceLease.hpp`, `session/manage.py`, `session/KeepActive.cpp` | Warm-up supervision, boot-bound lease audit, joined recovery and persistent silent output client |

Some original lab names remain, including `HalSilenceDriver.cpp` and `Pro40
Controller Lab.app`; the session profile carries application audio. The code
reuses ASFW's existing `CipHeader` and `DbcCounter`. It must not be merged into
ASFW's payload-opaque transport layers as-is.

## Build and offline checks

Use a Mac with Xcode command-line tools and an SDK containing the native
FireWire APIs. From the repository root:

```sh
bash tools/pro40-sequoia/check-maintenance-update.sh
bash tools/pro40-sequoia/check-startup-preroll.sh
python3 -B tools/pro40-sequoia/check-duplex-worker-processes.py --build-only --native-worker
```

These commands run hardware-free checks or compile generated fixtures. They do
not install a driver, start FireWire or restart CoreAudio. The maintenance suite
covers speaker storage, twelve-channel flow, delivery scheduling, startup,
lease recovery, teardown and Python supervision. Fixture `--run` mode is a
separate explicit operation that registers a temporary user job; it is not
needed for the build-only checks above.

For a static development package, set `PRO40_SIGNING_IDENTITY` to an available
Apple Development certificate SHA-1 and `PRO40_SIGNING_TEAM` to its team ID:

```sh
python3 tools/pro40-sequoia/controller-lab/check-package.py \
  --profile on-demand --signed-namespace \
  --signing-identity "$PRO40_SIGNING_IDENTITY" \
  --team-identifier "$PRO40_SIGNING_TEAM" \
  --enable-original-peer-diagnostic --enable-gated-worker-diagnostic \
  --duplex-hardware --duplex-delegated --session-profile \
  --real-guid 00130e0400000001
python3 tools/pro40-sequoia/hal-silence/build-delegated-host.py PACKAGE_DIRECTORY --hardware
```

The GUID is a **synthetic fixture identity**. These commands build and inspect
an uninstalled package/HAL; they do not register or start it. Exact development
identity pins are generated locally and must not be replaced with wildcards.

**Portable deployment remains unfinished.** Session management and login warm-up
source are supplied for review, not as a turnkey installer. Their fixed identity,
lease path, signing and once-per-boot policies require deliberate adaptation.
Changing only `--real-guid` does not port the whole contract. Private commissioning
recipes, historical receipts and the working machine's signed packages are not
distributed. No ready-to-install binary or certificate is supplied.

## Limits

- Cold input mute still relies on a finite, input-discarding login warm-up.
  New clock/lease handling fixes specific startup blockers, not the root cause
  of the underlying cold-input behavior. Failed current-boot attempts are not retried.
- An earlier physical round-trip measurement was approximately **225.8 ms**.
  The 15 ms warm-client opening measurement is not audio latency. Direct
  monitoring remains the practical microphone path.
- No controlled three-hour run, systematic sleep/wake or hot-unplug qualification,
  portable installation, other interface/rate/clock qualification or MIDI support
  is claimed. Exposing channels 7–12 does not establish their physical routing.
- The latest observed automatic login found an already-clean lease. Previous-boot
  recovery logic has software coverage and a related manual idle-audit hardware
  result, but automatic recovery of a stamped stale active lease still needs
  its own hardware observation.

See [startup](STARTUP.md), [session lifecycle](SESSION_LIFECYCLE.md) and
[warm-up contract](login-warmup/README.md) for the specific guards.

## Source and evidence provenance

This is a selected source snapshot from the development checkout, preserving the
existing public branch history. No raw audio, device captures, private host paths,
signing material, installed binaries or reference-stack implementations are
included. Tests use generated data and synthetic identities. Linux and Apple
sources were consulted for behavior, not copied. The repository's Apache-2.0
license applies. All changes remain under `tools/pro40-sequoia/`.
