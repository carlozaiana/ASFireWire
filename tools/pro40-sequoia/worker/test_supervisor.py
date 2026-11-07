"""Real child-process tests; every child is a hardware-free fixture."""
import sys
import tempfile
import time
import unittest
from pathlib import Path
from supervisor import Supervisor

FAKE = Path(__file__).with_name('fake_worker.py')


class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pro40-worker-')
        self.root = Path(self.temp.name)
        self.children = []
        self.release = self.root/'release'

    def tearDown(self):
        # Fixtures only: allow holds to end and request cooperative stops.
        # No kill is used, even by the test cleanup.
        self.release.touch()
        for child in self.children:
            if child.process:
                child.request_stop()
                self.until(child, lambda: child.state == 'closed')
        self.temp.cleanup()

    def launch(self, mode, supervisor=None, **kwargs):
        s = supervisor or Supervisor(**kwargs)
        s.start([sys.executable, str(FAKE), mode, str(self.release)],
                self.root/f'diagnostic-{len(self.children)}.txt')
        self.children.append(s)
        return s

    def until(self, s, predicate):
        deadline = time.monotonic()+5
        while not predicate():
            s.poll()
            if time.monotonic() > deadline:
                self.fail(f'fixture did not settle: {s.state}')
            time.sleep(.002)

    def test_completed_and_explicit_reuse(self):
        s = self.launch('complete')
        self.until(s, lambda: s.state == 'closed')
        self.assertFalse(s.blocked)
        self.assertEqual(s.completion['process_exit'], 0)
        self.launch('complete', supervisor=s)
        self.until(s, lambda: s.state == 'closed')
        self.assertFalse(s.blocked)

    def test_pending_stop_waits_for_ready(self):
        s = self.launch('wait-stop')
        s.request_stop()
        self.assertFalse(s.signaled)
        self.until(s, lambda: s.state == 'closed')
        self.assertTrue(s.signaled)
        self.assertFalse(s.blocked)
        self.assertEqual(s.completion['result']['outcome']['reason'], 'interrupted')
        previous = s.completion
        self.launch('complete', supervisor=s)
        self.until(s, lambda: s.state == 'closed')
        self.assertFalse(s.signaled)
        self.assertFalse(s.blocked)
        self.assertEqual(s.completion['process_exit'], 0)
        self.assertEqual(previous['result']['outcome']['reason'], 'interrupted')

    def test_result_not_accepted_until_process_exits(self):
        s = self.launch('result-hold')
        self.until(s, lambda: s.result is not None)
        self.assertIsNone(s.completion)
        self.assertIsNotNone(s.process)
        with self.assertRaises(RuntimeError):
            self.launch('complete', supervisor=s)
        self.release.touch()
        self.until(s, lambda: s.state == 'closed')
        self.assertFalse(s.blocked)

    def test_faults_block_restart(self):
        for mode in ('missing', 'signal', 'malformed', 'truncated', 'flood',
                     'duplicate-key', 'duplicate-result', 'wrong-exit',
                     'wrong-acceptance', 'wrong-type', 'cleanup'):
            with self.subTest(mode=mode):
                s = self.launch(mode)
                self.until(s, lambda: s.state == 'closed')
                self.assertTrue(s.blocked)
                self.assertLessEqual(len(s.buffer), 4096)
                with self.assertRaises(RuntimeError):
                    self.launch('complete', supervisor=s)

    def test_deadline_does_not_kill_or_release_live_child(self):
        s = self.launch('hold', run_seconds=.1, grace_seconds=.05)
        self.until(s, lambda: s.ready)
        self.until(s, lambda: s.state == 'needs-attention')
        self.assertTrue(s.signaled)
        self.assertIsNone(s.process.poll())
        self.assertIsNone(s.completion)
        with self.assertRaises(RuntimeError):
            self.launch('complete', supervisor=s)
        self.release.touch()
        self.until(s, lambda: s.state == 'closed')
        self.assertFalse(s.blocked) # Late verified cleanup, not unknown death.

    def test_unready_deadline_never_signals(self):
        s = self.launch('unready', run_seconds=.1, grace_seconds=.05)
        self.until(s, lambda: s.state == 'needs-attention')
        self.assertFalse(s.signaled)
        self.assertIsNone(s.process.poll())
        self.release.touch()
        self.until(s, lambda: s.state == 'closed')
        self.assertTrue(s.blocked)

    def test_pre_start_result_and_spawn_failure(self):
        s = self.launch('pre-start')
        self.until(s, lambda: s.state == 'closed')
        self.assertFalse(s.blocked)
        self.assertIsNone(s.completion['result']['outcome'])
        with self.assertRaises(FileNotFoundError):
            s.start([str(self.root/'absent-executable')], self.root/'spawn-error.txt')
        self.assertIsNone(s.process)

    def test_invalid_options_do_not_spawn(self):
        for value in (0, -1, float('inf'), float('nan'), True):
            with self.assertRaises(ValueError):
                Supervisor(run_seconds=value)
        s = Supervisor()
        with self.assertRaises(ValueError):
            s.start('not an argument list', self.root/'invalid.txt')
        self.assertFalse((self.root/'invalid.txt').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
