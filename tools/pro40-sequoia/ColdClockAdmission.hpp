// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace Pro40 {
// Admission to an owned 48 kHz initialization, never permission to stream at
// the observed rate. Linux dice-interface.h:91-134 separates clock selection
// from actual locked rate; dice-stream.c:289-301 reselects after ownership.
// Hardware on 2026-10-19: internal selected48k, actual locked44.1k after use
// with the older Mac. All subsequent streaming/cleanup checks still require48k.
constexpr bool cold48kClock(uint32_t selected,uint32_t status,uint32_t measured) {
    return (selected==0x10c || selected==0x20c) && ((status==0x201 && measured==48000) ||
                             (status==0x101 && measured==44100));
}
}
