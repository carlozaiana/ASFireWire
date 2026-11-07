// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
namespace Pro40Audio {
// Installed full-duplex profile at 48 kHz. These are stream channels, not
// promises about physical connectors: MixControl owns the device router.
// Stream A has twelve PCM slots followed by MIDI; stream B remains silent.
// Cross-checked: references/alsa-userspace-control-protocols-impl/protocols/
// dice/src/focusrite/spro40.rs:24-46 and linux-dice/amdtp-am824.c:99-102,167-173.
inline constexpr size_t duplexInputChannels=8;
inline constexpr size_t duplexOutputChannels=12;
}
