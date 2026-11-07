#!/bin/bash
# Hardware-free source-export validation; no installed HAL, launchd or device operations.
set -euo pipefail
cd "$(dirname "$0")/../.."
bash tools/pro40-sequoia/check-speaker-assignments.sh
bash tools/pro40-sequoia/check-duplex-delivery.sh
bash tools/pro40-sequoia/check-duplex-connection.sh
bash tools/pro40-sequoia/check-duplex-output-startup.sh
out=build/pro40-sequoia/maintenance-update
mkdir -p "$out"
for test in worker/DeviceLeaseChecks worker/RecoveryAuditChecks DmaTeardownChecks session/KeepActiveChecks; do
  name="${test##*/}"
  xcrun clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    "tools/pro40-sequoia/$test.cpp" -framework IOKit -framework CoreAudio -framework CoreFoundation \
    -o "$out/$name"
  "$out/$name"
done
python3 -B tools/pro40-sequoia/login-warmup/test_login_warmup.py
python3 -B tools/pro40-sequoia/worker/test_supervisor.py
python3 -B tools/pro40-sequoia/worker/test_session_control.py
python3 -B tools/pro40-sequoia/session/check-manage.py
