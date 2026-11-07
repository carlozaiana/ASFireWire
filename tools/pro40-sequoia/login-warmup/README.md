# Login warm-up orchestration source

This is reviewable source for the development host's existing startup workaround,
not a redistributable installer. `run_login_warmup.py` uses synthetic fixture GUID
`00130e0400000001` in this public export. No signed worker package, launch-agent
installer, development signing identity or historical machine receipt is supplied.
Do not run it as a hardware setup command or assume that changing its GUID alone
ports the installation, lease and keeper contracts.

The installed behavior being described waits 30 seconds at login, identifies
the original Pro40, checks a sealed package and runs a single finite 65536-cycle
warm-up. Input samples are discarded and outputs are silent. Completion requires
native transfer/evidence/cleanup success and a collected child result. Unknown
cleanup remains blocked, and a consumed boot is never silently retried.

The revision-004 manifest adds the signed `pro40-recover` helper and recovery mode
`previous-boot-idle-audit-v1`. Before native entry, the helper performs a read-only
hardware audit under exclusive lease ownership. Recovery of an unclean marker
requires proven earlier-Mac-boot identity plus a matching active-file mtime;
same-boot, unstamped and ambiguous state is refused. Clean existing leases still
require an idle hardware audit. Missing leases follow the ordinary acquisition
path. This code does not prove other interface power/reset states are safe.

The original cold-input mute cause remains unresolved. A successful warm-up
means its finite transfer and cleanup passed, not that microphone signal was
measured. `input_saved` and `input_signal_verified` remain false. The keeper waits
for genuine current-boot completion before holding the normal CoreAudio stream.

Run the hardware-free tests from the repository root:

```sh
python3 -B tools/pro40-sequoia/login-warmup/test_login_warmup.py
python3 -B tools/pro40-sequoia/worker/test_supervisor.py
python3 -B tools/pro40-sequoia/worker/test_session_control.py
```

Tests inject registry/audit/native outcomes or use fake child processes. They do
not create a real warm-up package, open FireWire or alter launchd services.
