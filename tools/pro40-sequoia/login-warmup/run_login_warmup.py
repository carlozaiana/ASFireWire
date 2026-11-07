#!/usr/bin/env python3
"""One supervised, input-discarding Pro40 initialization per Mac boot."""
import hashlib
import json
import os
import plistlib
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True
GUID = '00130e0400000001'
FILES = {'pro40-native-worker', 'session_control.py', 'supervisor.py', 'run_login_warmup.py'}
RECOVERY_MODE = 'previous-boot-idle-audit-v1'


def write_status(path, value):
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, prefix='.status-', delete=False) as f:
        temporary = Path(f.name)
        json.dump(value, f, indent=2)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    os.replace(temporary, path)


def verify_package(package):
    manifest = json.loads((package / 'manifest.json').read_text())
    mode = manifest.get('recovery_mode')
    expected = FILES | ({'pro40-recover'} if mode == RECOVERY_MODE else set())
    if mode not in (None, RECOVERY_MODE) or manifest.get('schema') != 1 or manifest.get('guid') != GUID or set(manifest.get('files', {})) != expected:
        raise ValueError('unsupported warm-up package')
    for name, digest in manifest['files'].items():
        p = package / name
        if p.is_symlink() or hashlib.sha256(p.read_bytes()).hexdigest() != digest:
            raise ValueError('warm-up package changed: '+name)
    subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(package / 'pro40-native-worker')],
                   capture_output=True, check=True, timeout=10)
    if mode:
        subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(package / 'pro40-recover')],
                       capture_output=True, check=True, timeout=10)


def audit_before_warmup(package, attempt):
    """The native helper holds flock across provenance and two idle snapshots."""
    lease = Path.home() / 'Library/Application Support/ASFW-Pro40-Workers' / (GUID + '.lock')
    try:
        lease.lstat()
    except FileNotFoundError:
        return dict(result='no-existing-lease', hardware_accessed=False)
    result = subprocess.run([str(package / 'pro40-recover'), '--guid', GUID, '--log',
                             str(attempt / 'recovery-audit.log'), '--after-reboot'],
                            capture_output=True, text=True, timeout=15)
    receipt = dict(exit=result.returncode, stdout=result.stdout, stderr=result.stderr)
    write_status(attempt / 'recovery-audit.json', receipt)
    if result.returncode or not re.search(r'^Recovery: (recovered|already-clean)\.', result.stdout, re.M):
        raise RuntimeError('Idle/previous-boot audit refused; safety lock retained. See recovery-audit.json.')
    return receipt


def present(entries):
    if not isinstance(entries, list):
        raise ValueError('invalid FireWire registry response')
    matches = [v for v in entries if isinstance(v, dict) and v.get('IOObjectClass') == 'IOFireWireDevice'
               and v.get('GUID') == int(GUID, 16) and v.get('Vendor_ID') == 0x130e and v.get('Model_ID') == 5]
    if len(matches) > 1:
        raise ValueError('ambiguous Pro40 registry identity')
    return len(matches) == 1


def completed(snapshot, log, directory):
    c = snapshot.get('completion') or {}
    r = c.get('result') or {}
    accepted = (snapshot.get('state') == 'closed' and not snapshot.get('restart_blocked', True)
                and c.get('error') is None and c.get('process_exit') == 0 and r.get('exit') == 0
                and r.get('outcome') == dict(reason='completed', transfer=0, evidence=0, cleanup=0))
    required = ('warmup-policy version=1 cycles=65536 input=discarded retry=none',
                'duplex-result reason=completed transfer=00000000 evidence=00000000 cleanup=00000000',
                'session-lifecycle closed=1 accepted=1')
    return (accepted and all(log.splitlines().count(s) == 1 for s in required)
            and bool(re.search(r'^assigned-idle-window schema=1 started=1 state=1 origin-us=\d+ samples=6$', log, re.M))
            and bool(re.search(r'^assigned-idle-quiet-hold schema=1 started=1 state=1 ', log, re.M))
            and not any(s in log for s in ('input-raw-packet ', 'input1-level ', 'transmit-measured-failed-copy '))
            and not any(p.suffix in ('.wav', '.bin', '.csv') for p in directory.iterdir()))


def run_once(package, state, boot, registry, controller_factory, cancelled, pause=time.sleep,
             *, settle_seconds=30, monotonic=time.monotonic, preflight=None):
    """All dependencies are explicit for hardware-free tests; main fixes them."""
    if not re.fullmatch(r'[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}', boot):
        raise ValueError('invalid boot identifier')
    if type(settle_seconds) not in (int, float) or not 0 <= settle_seconds <= 30:
        raise ValueError('invalid settling interval')
    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    attempt = state / boot.upper()
    try:
        attempt.mkdir(mode=0o700)
    except FileExistsError:
        # An interrupted/failed attempt also consumes this boot. Never reset it
        # or create another native worker to recover an unknown cleanup state.
        return 0
    report = dict(schema=1, boot_uuid=boot.upper(), guid=GUID, status='checking',
                  directory=str(attempt), input_saved=False, input_signal_verified=False,
                  automatic_retry=False, started_unix=time.time(), settle_seconds=settle_seconds)

    def publish(status, **values):
        report.update(status=status, **values)
        write_status(attempt / 'status.json', report)
        write_status(state / 'latest.json', report)

    publish('checking')
    if cancelled():
        publish('cancelled', detail='Stopped before native entry.')
        return 2
    if settle_seconds:
        # Login competes with other startup work. This is a bounded scheduling
        # mitigation, not a hardware-readiness test or a second warm-up attempt.
        publish('waiting', detail='Waiting 30 seconds for login activity to settle; no audio session started.')
        deadline = monotonic() + settle_seconds
        while True:
            if cancelled():
                publish('cancelled', detail='Stopped during the login settling interval; no audio session started.')
                return 2
            remaining = deadline - monotonic()
            if remaining <= 0:
                break
            pause(min(.1, remaining))
    # IORegistry can return an empty/non-plist response early in login. Query
    # only after settling; there is still no native retry or hot-plug watcher.
    try:
        available = present(registry())
    except Exception as error:
        publish('failed', detail='Device detection failed after settling: '+str(error))
        return 2
    if not available:
        publish('skipped', detail='Pro40 absent after settling. No audio session started.')
        return 0
    if cancelled():
        publish('cancelled', detail='Stopped before native entry.')
        return 2
    if preflight:
        publish('auditing', detail='Checking idle state and previous-boot safety lock before native entry.')
        try:
            audit = preflight(attempt)
        except Exception as error:
            publish('failed', detail=str(error), native_entry=False, ended_unix=time.time())
            return 2
        report['preflight_audit'] = audit
        if cancelled():
            publish('cancelled', detail='Stopped after read-only audit, before native entry.')
            return 2
    controller = controller_factory([str(package / 'pro40-native-worker'), '--warm-up'], GUID)
    publish('initializing', detail='One finite warm-up; microphone samples are discarded.')
    try:
        controller.start(attempt / 'native')
    except Exception as error:
        publish('failed', detail='Worker could not start: '+str(error))
        return 2
    attention = False
    while True:
        if cancelled():
            controller.request_stop()
        snapshot = controller.poll()
        if snapshot['state'] == 'needs-attention' and not attention:
            attention = True
            try:
                publish('needs-attention', detail='Waiting for native cleanup; no replacement or retry is permitted.')
            except OSError:
                # A full disk must not drop ownership of a live native worker.
                print('Could not save status; continuing to supervise native cleanup.', file=sys.stderr)
        if snapshot['state'] == 'closed':
            break
        pause(.01)
    try:
        controller.save_summary()
        native = attempt / 'native'
        completion = snapshot.get('completion') or {}
        result = completion.get('result') or {}
        diagnostic = native / 'diagnostic.txt'
        if (completion.get('process_exit') == 75 and completion.get('error') is None
                and result.get('exit') == 75 and result.get('outcome') is None
                and diagnostic.is_file() and diagnostic.read_text().strip() in
                ('worker-device-admission: unclean', 'worker-device-admission: busy')):
            publish('failed', worker=snapshot, native_entry=False,
                    detail='Worker refused the device safety lock before hardware entry. No automatic retry.',
                    ended_unix=time.time())
            return 2
        log = (native / 'native.log').read_text()
        ok = completed(snapshot, log, native)
        publish('completed' if ok else 'failed', worker=snapshot,
                detail=('Warm-up and cleanup completed. Input signal has not been measured.' if ok
                        else 'Warm-up did not complete successfully. No automatic retry.'), ended_unix=time.time())
    except Exception as error:
        publish('failed', worker=snapshot, detail='Could not verify/save the result: '+str(error))
        return 2
    return 0 if ok else 2


def main():
    os.umask(0o077)
    package = Path(__file__).resolve().parent
    state = package.parent / 'state'
    stopped = False

    def stop(_signal, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    verify_package(package)
    boot = subprocess.run(['/usr/sbin/sysctl', '-n', 'kern.bootsessionuuid'], capture_output=True,
                          text=True, check=True, timeout=10).stdout.strip()

    def registry():
        result = subprocess.run(['/usr/sbin/ioreg', '-a', '-r', '-c', 'IOFireWireDevice'],
                                capture_output=True, check=True, timeout=10)
        return plistlib.loads(result.stdout)

    from session_control import SessionController
    manifest = json.loads((package / 'manifest.json').read_text())
    audit = (lambda attempt: audit_before_warmup(package, attempt)) if manifest.get('recovery_mode') else None
    return run_once(package, state, boot, registry, SessionController, lambda: stopped, preflight=audit)


if __name__ == '__main__':
    raise SystemExit(main())
