"""Single-threaded session control for the bounded native worker."""
import copy
import json
import re
import time
from pathlib import Path
from supervisor import Supervisor


class SessionController:
    """Explicit start/stop only. Keep polling until closed, even after a fault.

    Worker arguments are supplied by the application, never through a shell.
    Each run owns a fresh directory. Save its summary before another start.
    This is a diagnostic controller, not a continuous audio service.
    """
    def __init__(self, worker_argv, guid, *, run_seconds=45):
        if not re.fullmatch(r'[0-9a-fA-F]{16}', guid):
            raise ValueError('GUID must contain exactly 16 hexadecimal digits')
        if not isinstance(worker_argv, (list, tuple)) or not worker_argv or not all(
                isinstance(a, str) and a for a in worker_argv):
            raise ValueError('explicit worker command required')
        self._argv = list(worker_argv)
        self._guid = guid.lower()
        self._supervisor = Supervisor(run_seconds=run_seconds)
        self._pid = None
        self._elapsed = 0
        self._saved = True
        self.directory = None

    def start(self, directory):
        if self._supervisor.process is not None or self._supervisor.blocked:
            raise RuntimeError('session active or previous cleanup unknown')
        if not self._saved:
            raise RuntimeError('save the previous session summary before starting')
        directory = Path(directory).resolve()
        directory.mkdir(mode=0o700)  # Never reuse/overwrite a previous run.
        self._supervisor.start(self._argv + ['--guid', self._guid,
                               str(directory/'native.log')], directory/'diagnostic.txt')
        self.directory = directory
        self._pid = self._supervisor.process.pid  # Capture before poll releases it.
        self._started = time.monotonic()
        self._elapsed = 0
        self._saved = False

    def request_stop(self):
        self._supervisor.request_stop()

    def poll(self):
        active = self._supervisor.process is not None
        self._supervisor.poll()
        if active:
            self._elapsed = time.monotonic()-self._started
        return self.snapshot()

    def snapshot(self):
        s = self._supervisor
        completion = getattr(s, 'completion', None)
        # ready confirms signal-handler installation, not audio startup.
        return copy.deepcopy(dict(
            state=s.state, pid=self._pid, elapsed_seconds=self._elapsed,
            ready=getattr(s, 'ready', False), signaled=getattr(s, 'signaled', False),
            protocol_bytes=getattr(s, 'total', 0), completion=completion,
            restart_blocked=s.blocked, summary_saved=self._saved))

    def save_summary(self):
        if self._supervisor.state != 'closed':
            raise RuntimeError('summary requires process exit and protocol closure')
        if self._saved:
            return self.directory/'runner.json'
        target = self.directory/'runner.json'
        # Disk failure leaves the in-memory result and prevents another start.
        summary = self.snapshot()
        # This flag describes the live controller, not the recorded session.
        del summary['summary_saved']
        with target.open('x') as output:
            json.dump(summary, output, indent=2)
            output.write('\n')
        self._saved = True
        return target
