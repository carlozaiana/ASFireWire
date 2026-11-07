"""Hardware-free process fixtures. Never imports or launches the native runner."""
import json
import signal
import sys
import time
from pathlib import Path

mode = sys.argv[1]
stopped = False


def stop_handler(_signal, _frame):
    global stopped
    stopped = True


def emit(frame):
    print(json.dumps(frame), flush=True)


signal.signal(signal.SIGINT, stop_handler)
if mode == 'unready':
    while not Path(sys.argv[2]).exists():
        time.sleep(.005)
    sys.exit(0)
emit({'version': 1, 'type': 'ready'})
if mode == 'missing':
    sys.exit(3)
if mode == 'signal':
    signal.raise_signal(signal.SIGTERM)
if mode == 'malformed':
    print('{bad json', flush=True)
    sys.exit(2)
if mode == 'truncated':
    sys.stdout.write('{');sys.stdout.flush()
    sys.exit(2)
if mode == 'flood':
    print('x'*20000, flush=True)
    sys.exit(2)
if mode == 'duplicate-key':
    print('{"version":1,"version":1,"type":"ready"}', flush=True)
    sys.exit(2)
if mode == 'wait-stop':
    while not stopped:
        time.sleep(.005)
if mode == 'hold':
    while not Path(sys.argv[2]).exists():
        time.sleep(.005)
outcome = dict(reason='completed', transfer=0, evidence=0, cleanup=0)
code = 0
if mode in ('wait-stop', 'hold'):
    outcome.update(reason='interrupted', transfer=-536870165)
    code = 2
if mode == 'cleanup':
    outcome.update(reason='error', cleanup=-1)
    code = 2
if mode == 'pre-start':
    outcome = None
    code = 64
frame = dict(version=1, type='result', exit=code, outcome=outcome)
if mode == 'wrong-acceptance':
    frame['outcome']['cleanup'] = -1
if mode == 'wrong-type':
    frame['exit'] = False
emit(frame)
if mode == 'duplicate-result':
    emit(frame)
if mode == 'result-hold':
    while not Path(sys.argv[2]).exists():
        time.sleep(.005)
sys.exit(2 if mode == 'wrong-exit' else code)
