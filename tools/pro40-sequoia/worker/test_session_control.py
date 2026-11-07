"""Application control tests using real hardware-free child processes."""
import sys
import tempfile
import time
import unittest
import json
from pathlib import Path
from session_control import SessionController

FAKE = Path(__file__).with_name('fake_worker.py')
GUID = '00130e0400000001'


class ControlTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.controllers = []

    def tearDown(self):
        (self.root/'release').touch()
        for c in self.controllers:
            c.request_stop()
            if c.snapshot()['state'] not in ('idle', 'closed'):
                self.until(c, lambda s:s['state']=='closed')
        self.temp.cleanup()

    def controller(self, mode):
        c = SessionController([sys.executable, str(FAKE), mode,
                               str(self.root/'release')], GUID)
        self.controllers.append(c)
        return c

    def until(self, c, predicate):
        deadline = time.monotonic()+5
        while True:
            s = c.poll()
            if predicate(s):return s
            if time.monotonic()>deadline:self.fail(str(s))
            time.sleep(.002)

    def test_final_identity_persistence_and_explicit_reuse(self):
        c = self.controller('complete')
        c.start(self.root/'first')
        pid = c.snapshot()['pid']
        final = self.until(c, lambda s:s['state']=='closed')
        self.assertEqual(final['pid'], pid)
        self.assertEqual(final['completion']['process_exit'], 0)
        self.assertEqual(c.poll(), final)
        final['completion']['process_exit'] = 99
        self.assertEqual(c.snapshot()['completion']['process_exit'], 0)
        with self.assertRaises(RuntimeError):c.start(self.root/'second')
        self.assertFalse((self.root/'second').exists())
        saved = json.loads(c.save_summary().read_text())
        self.assertEqual(saved['pid'], pid)
        self.assertEqual(saved['completion']['process_exit'], 0)
        c.start(self.root/'second')
        self.assertNotEqual(c.snapshot()['pid'], pid)
        self.until(c, lambda s:s['state']=='closed')
        c.save_summary()

    def test_stop_and_active_start_refusal(self):
        c = self.controller('wait-stop');c.start(self.root/'first')
        with self.assertRaises(RuntimeError):c.start(self.root/'second')
        with self.assertRaises(RuntimeError):c.save_summary()
        c.request_stop();c.request_stop()
        final = self.until(c, lambda s:s['state']=='closed')
        self.assertTrue(final['signaled'])
        self.assertFalse(final['restart_blocked'])
        self.assertEqual(final['completion']['result']['outcome']['reason'], 'interrupted')
        c.save_summary()

    def test_result_before_exit_is_not_completion(self):
        c = self.controller('result-hold');c.start(self.root/'first')
        self.until(c, lambda s:s['protocol_bytes']>40)
        self.assertIsNone(c.snapshot()['completion'])
        with self.assertRaises(RuntimeError):c.save_summary()
        (self.root/'release').touch()
        self.until(c, lambda s:s['state']=='closed');c.save_summary()

    def test_faults_remain_blocked_after_save(self):
        for mode in ('cleanup', 'missing', 'wrong-exit'):
            c = self.controller(mode);c.start(self.root/mode)
            self.until(c, lambda s:s['state']=='closed');c.save_summary()
            self.assertTrue(c.snapshot()['restart_blocked'])
            with self.assertRaises(RuntimeError):c.start(self.root/(mode+'-again'))

    def test_existing_paths_and_summary_failure_preserve_evidence(self):
        c = self.controller('complete')
        with self.assertRaises(FileExistsError):c.start(self.root)
        c.start(self.root/'first');self.until(c, lambda s:s['state']=='closed')
        (self.root/'first/runner.json').write_text('existing evidence')
        with self.assertRaises(FileExistsError):c.save_summary()
        self.assertEqual((self.root/'first/runner.json').read_text(), 'existing evidence')
        self.assertFalse(c.snapshot()['summary_saved'])
        self.assertIsNotNone(c.snapshot()['completion'])
        with self.assertRaises(RuntimeError):c.start(self.root/'second')


if __name__ == '__main__':unittest.main(verbosity=2)
