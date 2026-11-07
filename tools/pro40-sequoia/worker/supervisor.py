"""Bounded diagnostic worker supervisor with a parent-owned liveness pipe."""
import json
import math
import os
import signal
import subprocess
import time


class Supervisor:
    """One owned child. poll() never waits or kills; start() never retries."""
    def __init__(self, run_seconds=45, grace_seconds=8):
        if not all(type(v) in (int, float) and math.isfinite(v) and v > 0
                   for v in (run_seconds, grace_seconds)):
            raise ValueError('deadlines must be positive')
        self.run_seconds, self.grace_seconds = run_seconds, grace_seconds
        self.process = None
        self.blocked = False
        self.state = 'idle'

    def start(self, argv, diagnostic_path):
        if self.process is not None or self.blocked:
            raise RuntimeError('worker still owned or previous cleanup unknown')
        if not isinstance(argv, (tuple, list)) or not argv or not all(isinstance(a, str) and a for a in argv):
            raise ValueError('explicit executable and argument list required')
        diagnostic = open(diagnostic_path, 'xb')
        try:
            process = subprocess.Popen(argv, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=diagnostic,
                                       close_fds=True, shell=False)
        except BaseException:
            diagnostic.close()
            raise
        self.process, self.diagnostic = process, diagnostic
        os.set_blocking(process.stdout.fileno(), False)
        self.started = time.monotonic()
        self.stop_at = None
        self.stop_pending = False
        self.signaled = False
        self.ready = False
        self.eof = False
        self.buffer = bytearray()
        self.total = 0
        self.result = None
        self.error = None
        self.completion = None
        self.state = 'running'

    def request_stop(self):
        if self.process is not None:
            self.stop_pending = True
            if self.stop_at is None:
                self.stop_at = time.monotonic()

    def _fault(self, message):
        if self.error is None:
            self.error = message
        self.request_stop()
        self.state = 'needs-attention'

    def _frame(self, line):
        # Exact schemas, no duplicate JSON keys or bool-as-integer ambiguity.
        def unique(pairs):
            d = {}
            for key, value in pairs:
                if key in d:
                    raise ValueError('duplicate key')
                d[key] = value
            return d
        try:
            frame = json.loads(line, object_pairs_hook=unique)
            if not isinstance(frame, dict) or type(frame.get('version')) is not int or frame['version'] != 1:
                raise ValueError('version')
            if frame.get('type') == 'ready':
                if set(frame) != {'version', 'type'} or self.ready or self.result is not None:
                    raise ValueError('ready order')
                self.ready = True
                return
            if set(frame) != {'version', 'type', 'exit', 'outcome'} or frame['type'] != 'result' or not self.ready or self.result is not None:
                raise ValueError('result order/schema')
            code, outcome = frame['exit'], frame['outcome']
            if type(code) is not int:
                raise ValueError('exit type')
            if outcome is None:
                if code not in (1, 64, 75):
                    raise ValueError('pre-start exit')
            else:
                if not isinstance(outcome, dict) or set(outcome) != {'reason', 'transfer', 'evidence', 'cleanup'}:
                    raise ValueError('outcome schema')
                if outcome['reason'] not in ('completed', 'interrupted', 'error', 'watchdog', 'invalid-notification'):
                    raise ValueError('reason')
                if any(type(outcome[k]) is not int or not -(2**31) <= outcome[k] < 2**31 for k in ('transfer', 'evidence', 'cleanup')):
                    raise ValueError('status fields')
                accepted = outcome['reason'] == 'completed' and all(outcome[k] == 0 for k in ('transfer', 'evidence', 'cleanup'))
                if code != (0 if accepted else 2):
                    raise ValueError('acceptance mismatch')
            self.result = frame
        except (ValueError, TypeError, UnicodeError, RecursionError):
            self._fault('invalid worker protocol')

    def poll(self):
        if self.process is None:
            return self.state
        # Drain at most32KiB per call. Retain at most4KiB per worker, including
        # framing. On a fault continue bounded draining rather than closing a
        # live worker's pipe or forcing it to terminate via SIGPIPE.
        for _ in range(8):
            try:
                data = os.read(self.process.stdout.fileno(), 4096)
            except BlockingIOError:
                break
            if not data:
                self.eof = True
                if self.buffer:
                    self._fault('truncated worker protocol')
                    self.buffer.clear()
                break
            self.total += len(data)
            if self.total > 4096:
                self.buffer.clear()
                self._fault('worker protocol exceeds bound')
                continue
            if self.error:
                continue
            self.buffer.extend(data)
            while b'\n' in self.buffer and not self.error:
                line, _, rest = self.buffer.partition(b'\n')
                self.buffer = bytearray(rest)
                self._frame(line)
        code = self.process.poll()
        now = time.monotonic()
        if now-self.started >= self.run_seconds:
            self.request_stop()
        # ready attests the worker installed its cooperative SIGINT handler.
        # Before ready, a stop is queued; sending SIGINT could otherwise kill it.
        if code is None and self.stop_pending and self.ready and not self.signaled and self.result is None:
            self.process.send_signal(signal.SIGINT)
            self.signaled = True
        if self.stop_at is not None and now-self.stop_at >= self.grace_seconds:
            self.state = 'needs-attention'
        elif self.stop_pending and not self.error:
            self.state = 'stopping'
        if code is not None and self.eof:
            if self.result is None or code != self.result['exit']:
                self._fault('missing result or exit mismatch')
            cleanup = self.result['outcome']['cleanup'] if self.result and self.result['outcome'] else 0
            self.blocked = self.error is not None or cleanup != 0
            self.completion = {'process_exit': code, 'result': self.result,
                               'error': self.error, 'restart_blocked': self.blocked}
            self.process.stdout.close()
            self.process.stdin.close()
            self.diagnostic.close()
            self.process = None
            self.state = 'closed'
        return self.state
