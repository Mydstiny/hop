import os
import unittest
from hop_linux.shortcut import ShortcutError, ShortcutTranslator

@unittest.skipUnless(os.environ.get('HOP_TEST_SHORTCUT_LIBRARY'), 'requires shared shortcut runtime')
class ShortcutTests(unittest.TestCase):
    def translator(self, source, target): return ShortcutTranslator(os.environ['HOP_TEST_SHORTCUT_LIBRARY'],source,target)
    def test_mac_command_to_harmony_control_order(self):
        t=self.translator(1,4)
        try:
            self.assertEqual(t.push(usage=0xe3,modifiers=64,down=1,repeat=0),[])
            output=t.push(usage=6,modifiers=64,down=1,repeat=0)
            self.assertEqual([(x['usage'],x['modifiers'],x['down']) for x in output],[(0xe0,4,1),(6,4,1)])
            self.assertEqual(t.push(usage=6,modifiers=64,down=0,repeat=0)[0]['usage'],6)
            self.assertEqual(t.push(usage=0xe3,modifiers=0,down=0,repeat=0)[0]['usage'],0xe0)
        finally:t.close()
    def test_linux_control_to_mac_command_caps_and_release(self):
        t=self.translator(3,1)
        try:
            self.assertEqual(t.push(usage=0xe4,modifiers=8|256,down=1,repeat=0),[])
            output=t.push(usage=0x19,modifiers=8|256,down=1,repeat=0)
            self.assertEqual([(x['usage'],x['modifiers']) for x in output],[(0xe7,128|256),(0x19,128|256)])
            output=t.release();self.assertEqual([(x['usage'],x['down']) for x in output],[(0x19,0),(0xe7,0)])
        finally:t.close()
    def test_pointer_flush_and_error_fail_closed(self):
        t=self.translator(1,2)
        try:
            t.push(usage=0xe3,modifiers=64,down=1,repeat=0)
            self.assertEqual(t.flush_pointer()[0]['usage'],0xe3)
            with self.assertRaises(ShortcutError):t.push(usage=0,modifiers=0,down=1,repeat=0)
            with self.assertRaises(ShortcutError):t.flush_pointer()
        finally:t.close()

    def test_capture_pump_preserves_modifier_and_pointer_order(self):
        from types import SimpleNamespace
        from hop_linux.x11_host_process import X11HostProcess
        from test_x11_host import FakeSession
        t=self.translator(3,1)
        host=X11HostProcess(screen_id=1,target_size=(100,100),capture_center=(50,50),translator=t)
        session=FakeSession();session.phase='remote';session.transport=SimpleNamespace(authenticated=True)
        host.session=session;host._tick=lambda:None
        host.events.put(('key',dict(usage=0xe0,modifiers=4,down=1,repeat=0)))
        host.events.put(('key',dict(usage=6,modifiers=4,down=1,repeat=0)))
        host.events.put(('key',dict(usage=6,modifiers=4,down=0,repeat=0)))
        host.events.put(('key',dict(usage=0xe0,modifiers=0,down=0,repeat=0)))
        try:
            host.pump(session)
            self.assertEqual([(event,fields['usage'],fields['down']) for event,fields in session.events],
                             [('key',0xe3,1),('key',6,1),('key',6,0),('key',0xe3,0)])
        finally:host.restore_local()
