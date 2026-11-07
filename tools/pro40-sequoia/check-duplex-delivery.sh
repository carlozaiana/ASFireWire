#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/pro40-sequoia/duplex-delivery
mkdir -p "$out"
for sanitizer in address,undefined thread; do
  xcrun clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize="$sanitizer" \
    tools/pro40-sequoia/DuplexDeliverySchedulingChecks.cpp -o "$out/checks-$sanitizer"
  "$out/checks-$sanitizer"
done
