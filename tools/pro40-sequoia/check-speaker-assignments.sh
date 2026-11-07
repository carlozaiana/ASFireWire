#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/pro40-sequoia/speaker-assignments
mkdir -p "$out"
flags=(-std=c++20 -O1 -g -Wall -Wextra -Werror)
for sanitizer in address,undefined thread; do
  xcrun clang++ "${flags[@]}" "-fsanitize=$sanitizer" tools/pro40-sequoia/HalSpeakerLayoutChecks.cpp \
    -framework CoreAudio -framework CoreFoundation -framework AudioToolbox -o "$out/layout-checks-$sanitizer"
  "$out/layout-checks-$sanitizer"
done
xcrun clang++ "${flags[@]}" -fsanitize=address,undefined \
  tools/pro40-sequoia/HalSilenceDriverChecks.cpp tools/pro40-sequoia/HalSilenceDriver.cpp \
  -framework CoreAudio -framework CoreFoundation -o "$out/driver-checks"
"$out/driver-checks"
