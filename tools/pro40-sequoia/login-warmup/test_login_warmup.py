"""Hardware-free orchestration/failure tests. No native library is loaded."""
import json
import tempfile
import unittest
from pathlib import Path
from run_login_warmup import GUID, completed, present, run_once

BOOT = '11111111-2222-3333-4444-555555555555'
DEVICE = {'IOObjectClass': 'IOFireWireDevice', 'GUID': int(GUID, 16), 'Vendor_ID': 0x130e, 'Model_ID': 5}
LOG = '''warmup-policy version=1 cycles=65536 input=discarded retry=none
duplex-result reason=completed transfer=00000000 evidence=00000000 cleanup=00000000
session-lifecycle closed=1 accepted=1
assigned-idle-window schema=1 started=1 state=1 origin-us=10 samples=6
assigned-idle-quiet-hold schema=1 started=1 state=1 cleanup-origin-us=1 begin-us=2 end-us=3
'''


def snapshot(code=0, blocked=False):
    return dict(state='closed', restart_blocked=blocked, completion=dict(error=None, process_exit=code,
        result=dict(exit=code, outcome=dict(reason='error' if code else 'completed', transfer=code, evidence=0, cleanup=0))))


class LoginTests(unittest.TestCase):
    def test_identity(self):
        self.assertTrue(present([DEVICE]))
        self.assertFalse(present([]))
        for key in ('GUID', 'Vendor_ID', 'Model_ID', 'IOObjectClass'):
            self.assertFalse(present([{**DEVICE, key: 0}]))
        with self.assertRaises(ValueError): present([DEVICE, DEVICE])

    def test_one_attempt_and_no_signal_claim(self):
        for scenario in ('absent', 'cancelled', 'complete', 'transfer', 'unknown', 'attention', 'spawn', 'registry', 'missing-log', 'payload'):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                calls = []

                class Controller:
                    def __init__(self, argv, guid):
                        calls.append(('create', argv, guid));self.polls=0
                    def start(self, directory):
                        calls.append('start')
                        if scenario == 'spawn': raise OSError('injected spawn failure')
                        directory.mkdir();self.directory=directory
                        if scenario != 'missing-log':
                            (directory/'native.log').write_text(LOG+('input-raw-packet forbidden\n' if scenario == 'payload' else ''))
                    def request_stop(self): calls.append('stop')
                    def poll(self):
                        self.polls+=1
                        if scenario == 'attention' and self.polls<3:return dict(state='needs-attention')
                        return snapshot(2 if scenario in ('transfer','unknown') else 0, scenario == 'unknown')
                    def save_summary(self): calls.append('summary')

                def registry():
                    if scenario == 'registry': raise OSError('injected registry failure')
                    return [] if scenario == 'absent' else [DEVICE]

                run_once(root,root/'state',BOOT,registry,Controller,lambda:scenario=='cancelled',lambda _:None,settle_seconds=0)
                report=json.loads((root/'state/latest.json').read_text())
                self.assertFalse(report['input_saved'])
                self.assertFalse(report['input_signal_verified'])
                self.assertFalse(report['automatic_retry'])
                self.assertEqual(report['status']=='completed',scenario in ('complete','attention'))
                before=list(calls)
                run_once(root,root/'state',BOOT,lambda:[DEVICE],Controller,lambda:False,lambda _:None,settle_seconds=0)
                self.assertEqual(calls,before) # Every result, including failure/absence, consumes the boot.
                if scenario in ('absent','cancelled','registry'):self.assertEqual(calls,[])
                else:self.assertEqual(calls.count('start'),1)

    def test_login_settling_is_cancellable_and_rechecks_presence(self):
        for scenario in ('complete','cancelled','absent','recheck-failure','absent-at-login'):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp); now=[0.0]; reads=[]; starts=[]
                class Controller:
                    def __init__(self, argv, guid):
                        starts.append(now[0])
                    def start(self, directory):
                        directory.mkdir();(directory/'native.log').write_text(LOG)
                    def poll(self):return snapshot()
                    def save_summary(self):pass
                def registry():
                    reads.append(now[0])
                    if scenario=='recheck-failure':raise OSError('registry failed')
                    return [] if scenario in ('absent-at-login','absent') else [DEVICE]
                def pause(seconds):
                    self.assertGreater(seconds,0); self.assertLessEqual(seconds,.1)
                    self.assertEqual(starts,[])
                    self.assertEqual(json.loads((root/'state/latest.json').read_text())['status'],'waiting')
                    now[0]+=seconds
                run_once(root,root/'state',BOOT,registry,Controller,
                         lambda:scenario=='cancelled' and now[0]>=5,pause,monotonic=lambda:now[0])
                report=json.loads((root/'state/latest.json').read_text())
                if scenario=='complete':
                    self.assertEqual(starts,[30.0]);self.assertEqual(reads,[30.0])
                    self.assertEqual(report['status'],'completed')
                else:
                    self.assertEqual(starts,[])
                    self.assertEqual(report['status'],{'cancelled':'cancelled','absent':'skipped',
                        'recheck-failure':'failed','absent-at-login':'skipped'}[scenario])
                if scenario=='absent-at-login':self.assertEqual(now[0],30)
                if scenario=='cancelled':self.assertLess(now[0],5.2)
                before=(now[0],len(reads),list(starts))
                run_once(root,root/'state',BOOT,registry,Controller,lambda:False,pause,monotonic=lambda:now[0])
                self.assertEqual((now[0],len(reads),starts),before)

    def test_audit_refusal_never_starts_worker_or_retries(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);calls=[]
            def audit(attempt):
                calls.append('audit')
                raise RuntimeError('same-boot or missing provenance')
            def forbidden(*args):self.fail('Worker must not start after refused audit')
            self.assertEqual(run_once(root,root/'state',BOOT,lambda:[DEVICE],forbidden,
                lambda:False,settle_seconds=0,preflight=audit),2)
            report=json.loads((root/'state/latest.json').read_text())
            self.assertFalse(report['native_entry'])
            self.assertEqual(report['status'],'failed')
            self.assertEqual(run_once(root,root/'state',BOOT,forbidden,forbidden,
                lambda:False,settle_seconds=0,preflight=audit),0)
            self.assertEqual(calls,['audit'])

    def test_successful_audit_precedes_native_entry_and_admission_failure_has_no_log(self):
        for denied in (False,True):
            with self.subTest(denied=denied),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);calls=[]
                def audit(attempt):calls.append('audit');return dict(exit=0)
                class Controller:
                    def __init__(self,*args):calls.append('create')
                    def start(self,directory):
                        calls.append('start');directory.mkdir()
                        if denied:(directory/'diagnostic.txt').write_text('worker-device-admission: unclean\n')
                        else:(directory/'native.log').write_text(LOG)
                    def save_summary(self):pass
                    def poll(self):
                        if not denied:return snapshot()
                        value=snapshot(75);value['completion']['result']['outcome']=None;return value
                self.assertEqual(run_once(root,root/'state',BOOT,lambda:[DEVICE],Controller,
                    lambda:False,settle_seconds=0,preflight=audit),2 if denied else 0)
                self.assertEqual(calls,['audit','create','start'])
                report=json.loads((root/'state/latest.json').read_text())
                if denied:
                    self.assertFalse(report['native_entry'])
                    self.assertIn('safety lock',report['detail'])
                    self.assertNotIn('No such file',report['detail'])
                else:self.assertEqual(report['status'],'completed')

    def test_reject_incomplete_or_saved_audio(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            self.assertTrue(completed(snapshot(),LOG,root))
            for line in LOG.splitlines():self.assertFalse(completed(snapshot(),LOG.replace(line+'\n',''),root))
            self.assertFalse(completed(snapshot(),LOG+LOG,root))
            for suffix in ('.wav','.bin','.csv'):
                p=root/('unexpected'+suffix);p.write_bytes(b'')
                self.assertFalse(completed(snapshot(),LOG,root));p.unlink()
            self.assertFalse(completed(snapshot(2),LOG,root))
            self.assertFalse(completed(snapshot(0,True),LOG,root))


if __name__=='__main__':unittest.main(verbosity=2)
