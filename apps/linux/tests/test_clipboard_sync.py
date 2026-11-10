import threading
import time
import unittest
from hop_linux.clipboard import TEXT,PNG
from hop_linux.clipboard_sync import ClipboardCoordinator

class ClipboardCoordinatorTests(unittest.TestCase):
    def test_revocation_barrier_waits_for_validator(self):
        from unittest.mock import patch
        entered=threading.Event();finish=threading.Event();c=ClipboardCoordinator();c.focus(1)
        def slow(*_):entered.set();finish.wait(2)
        try:
            with patch('hop_linux.clipboard_sync.validate_payload',slow):
                c.submit(1,TEXT,b'hello',0);self.assertTrue(entered.wait(1))
                c.focus(0);self.assertFalse(c.barrier_ready())
                finish.set();deadline=time.monotonic()+1
                while not c.barrier_ready() and time.monotonic()<deadline:time.sleep(.001)
                self.assertTrue(c.barrier_ready());self.assertEqual(c.poll(lambda:0,lambda *_:self.fail('stale data applied')),'empty')
        finally:finish.set();c.close()
    def test_revocation_barrier_waits_for_owner_callback(self):
        c=ClipboardCoordinator();c.focus(1);entered=threading.Event();finish=threading.Event()
        try:
            c.submit(1,TEXT,b'hello',0)
            def apply(*_):entered.set();finish.wait(2);return True
            def consume():
                while c.poll(lambda:0,apply)=='empty':time.sleep(.001)
            t=threading.Thread(target=consume);t.start();self.assertTrue(entered.wait(1))
            c.focus(0);self.assertFalse(c.barrier_ready())
            finish.set();t.join(2);self.assertFalse(t.is_alive());self.assertTrue(c.barrier_ready())
        finally:finish.set();c.close()
    def poll(self,c,version,applied):
        end=time.monotonic()+2
        while time.monotonic()<end:
            result=c.poll(lambda:version[0],lambda *value:applied.append(value))
            if result!='empty':return result
            time.sleep(.001)
        self.fail('clipboard worker timed out')
    def test_applies_only_matching_focus_and_version(self):
        c=ClipboardCoordinator();version=[1];applied=[]
        try:
            c.focus(7);c.submit(7,TEXT,b'hello',1)
            self.assertEqual(self.poll(c,version,applied),'applied');self.assertEqual(applied,[(7,TEXT,b'hello',1)])
            c.submit(7,TEXT,b'stale',1);version[0]=2
            self.assertEqual(self.poll(c,version,applied),'discarded')
            c.submit(7,TEXT,b'old focus',2);c.focus(8)
            self.assertEqual(self.poll(c,version,applied),'discarded');self.assertEqual(len(applied),1)
        finally:c.close()
    def test_invalid_content_and_png_without_decoder_rejected(self):
        c=ClipboardCoordinator();c.focus(1)
        try:
            c.submit(1,TEXT,b'\xff',0)
            self.assertEqual(self.poll(c,[0],[]),'discarded')
            with self.assertRaises(ValueError):c.submit(1,PNG,b'png',0)
            c.focus(0)
            with self.assertRaises(PermissionError):c.submit(1,TEXT,b'x',0)
        finally:c.close()
    def test_only_one_inflight_value(self):
        c=ClipboardCoordinator();c.focus(1)
        try:
            c.submit(1,TEXT,b'first',0)
            with self.assertRaises(RuntimeError):c.submit(1,TEXT,b'second',0)
            self.assertEqual(self.poll(c,[0],[]),'applied')
        finally:c.close()
