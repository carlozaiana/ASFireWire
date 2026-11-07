#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/pro40-sequoia/duplex-output-startup
mkdir -p "$out"
flags=(-std=c++20 -O2 -g -Wall -Wextra -Werror)
for sanitizer in address,undefined thread; do
  for test in DuplexOutputStartupChecks PlaybackQueueChecks; do
    xcrun clang++ "${flags[@]}" "-fsanitize=$sanitizer" "tools/pro40-sequoia/$test.cpp" \
      -framework CoreAudio -framework CoreFoundation -o "$out/$test-$sanitizer"
    "$out/$test-$sanitizer"
  done
done
xcrun clang++ "${flags[@]}" -fsanitize=address,undefined tools/pro40-sequoia/HalNativeOutputTimingChecks.cpp \
  -framework CoreAudio -framework CoreFoundation -o "$out/hal-native-timing-checks"
"$out/hal-native-timing-checks"
