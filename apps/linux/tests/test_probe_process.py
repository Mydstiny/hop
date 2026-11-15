import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hop_linux.probe_process import ProbeBusy, ProbeError, X11Probe  # noqa: E402

FAKE = textwrap.dedent('''
    import json, os, sys
    log = sys.argv[1]
    mode = sys.argv[2]
    with open(log, 'a') as f:
        f.write('start\\n')
    print(json.dumps({'type': 'ready'}), flush=True)
    for line in sys.stdin:
        request = json.loads(line)
        if mode == 'die':
            sys.exit(1)
        if mode == 'hang':
            import time; time.sleep(30)
        if request['op'] == 'warp':
            reply = {'ok': True}
        elif mode == 'busy' and request['clear']:
            reply = {'ok': False, 'busy': True}
        else:
            reply = {'ok': True, 'screens': [[1, 0, 0, 1920, 1080]]}
            if request['pointer']:
                reply['pointer'] = [5, 6]
        print(json.dumps(reply), flush=True)
''')


class ProbeProcessTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.script = Path(self.dir.name) / 'fake.py'
        self.script.write_text(FAKE)
        self.log = Path(self.dir.name) / 'log'

    def tearDown(self):
        self.dir.cleanup()

    def probe(self, mode, timeout=2.0):
        probe = X11Probe([sys.executable, '-I', str(self.script), str(self.log), mode], timeout=timeout)
        self.addCleanup(probe.close)
        return probe

    def starts(self):
        return self.log.read_text().count('start') if self.log.exists() else 0

    def test_one_child_answers_many_queries(self):
        probe = self.probe('ok')
        for _ in range(5):
            self.assertEqual(probe.snapshot()['screens'], [[1, 0, 0, 1920, 1080]])
        self.assertEqual(probe.snapshot(clear=True, pointer=True)['pointer'], [5, 6])
        probe.warp(10, 20)
        self.assertEqual(self.starts(), 1)

    def test_held_input_is_reported_without_restart(self):
        probe = self.probe('busy')
        with self.assertRaises(ProbeBusy):
            probe.snapshot(clear=True)
        self.assertEqual(probe.snapshot()['screens'][0][3], 1920)
        self.assertEqual(self.starts(), 1)

    def test_dead_child_is_replaced_once_then_reported(self):
        probe = self.probe('die')
        with self.assertRaises(ProbeError):
            probe.snapshot()
        self.assertEqual(self.starts(), 2)

    def test_hung_child_times_out(self):
        probe = self.probe('hang', timeout=0.3)
        with self.assertRaises(ProbeError):
            probe.snapshot()

    def test_pointer_needs_clear(self):
        with self.assertRaises(ValueError):
            self.probe('ok').snapshot(pointer=True)


if __name__ == '__main__':
    unittest.main()
