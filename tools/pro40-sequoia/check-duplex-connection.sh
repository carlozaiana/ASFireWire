#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/pro40-sequoia/duplex-connection
mkdir -p "$out"
for sanitizer in address,undefined thread; do
  xcrun clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize="$sanitizer" -DPRO40_HAL_DUPLEX_AUDIO=1 \
    tools/pro40-sequoia/DuplexConnectionChecks.cpp tools/pro40-sequoia/HalSilenceDriver.cpp \
    -framework CoreAudio -framework CoreFoundation -lbsm -o "$out/checks-$sanitizer"
  "$out/checks-$sanitizer"
done
