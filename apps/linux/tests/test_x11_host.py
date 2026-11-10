import time
import unittest
from hop_linux.x11_host import X11Host
from hop_linux.keymap import KeyMap
from hop_linux.x11 import Event
from hop_linux.profiles import PointerProfile

class FakeBackend:
    def __init__(self): self.pending=[];self.grabbed=False;self.closed=False
    def keymap(self): return KeyMap.from_names({38:'AC01',50:'LFSH',62:'RTSH',37:'LCTL',64:'LALT',9:'ESC'})
    def grab(self): self.grabbed=True
    def input_state_clear(self): return True
    def warp(self,*a): pass
    def events(self): result=self.pending[:];self.pending.clear();return result
    def ungrab(self): self.grabbed=False
    def close(self): self.closed=True

class FakeSession:
    def __init__(self):self.events=[];self.closed=False
    def send_input(self,event,**fields): self.events.append((event,fields))
    def close(self):self.closed=True

class HostTests(unittest.TestCase):
    def test_capture_coalesces_and_preserves_key_button_order(self):
        backend=FakeBackend();host=X11Host(screen_id=1,target_size=(1000,1000),capture_center=(500,500),backend_factory=lambda:backend)
        session=FakeSession();host.start(acknowledged=True,input_state_clear=True)
        try:
            backend.pending.extend([Event('motion',x=510,y=500),Event('motion',x=520,y=500),Event('key',code=50,down=True),Event('key',code=38,down=True),Event('key',code=38,down=False),Event('key',code=50,down=False),Event('button',code=8,down=True),Event('button',code=8,down=False)])
            time.sleep(.06);host.pump(session)
            self.assertEqual([v[0] for v in session.events],['move','key','key','key','key','button','button'])
            self.assertEqual(session.events[1][1]['modifiers'],1)
            self.assertEqual(session.events[3][1]['usage'],4)
            self.assertEqual(session.events[-2][1]['button'],4)
        finally:host.restore_local()
        self.assertFalse(backend.grabbed);self.assertTrue(backend.closed)
    def test_watchdog_restores_without_network_thread(self):
        backend=FakeBackend();host=X11Host(screen_id=1,target_size=(100,100),capture_center=(50,50),backend_factory=lambda:backend,watchdog=.1)
        host.start(acknowledged=True,input_state_clear=True);time.sleep(.2)
        self.assertFalse(backend.grabbed);s=FakeSession()
        with self.assertRaises(RuntimeError):host.pump(s)
        self.assertTrue(s.closed)
    def test_queue_overflow_is_not_silent(self):
        host=X11Host(screen_id=1,target_size=(1,1),capture_center=(0,0))
        for _ in range(256):host._push('key',{})
        with self.assertRaises(RuntimeError):host._push('key',{})
    def test_requires_explicit_activation(self):
        h=X11Host(screen_id=1,target_size=(1,1),capture_center=(0,0))
        with self.assertRaises(RuntimeError):h.start(acknowledged=False,input_state_clear=True)
    def test_emergency_chord_ungrabs_without_waiting_for_network(self):
        backend=FakeBackend();host=X11Host(screen_id=1,target_size=(100,100),capture_center=(50,50),backend_factory=lambda:backend)
        host.start(acknowledged=True,input_state_clear=True)
        backend.pending.extend([Event('key',code=k,down=True) for k in (50,37,64,9)])
        end=time.monotonic()+1
        while backend.grabbed and time.monotonic()<end:time.sleep(.01)
        self.assertFalse(backend.grabbed);self.assertIsNotNone(host.error)
        host.restore_local()
    def test_target_rate_coalesces_motion_and_flushes_before_key(self):
        backend=FakeBackend();host=X11Host(screen_id=1,target_size=(1000,1000),capture_center=(500,500),backend_factory=lambda:backend,profile=PointerProfile(forward_hz=60))
        session=FakeSession();host.start(acknowledged=True,input_state_clear=True)
        try:
            for i in range(12):
                backend.pending.append(Event('motion',x=501+i,y=500));time.sleep(.002);host.pump(session)
            before=len([e for e in session.events if e[0]=='move'])
            self.assertLessEqual(before,4)
            backend.pending.extend([Event('motion',x=510,y=500),Event('key',code=38,down=True),Event('key',code=38,down=False)])
            time.sleep(.03);host.pump(session)
            kinds=[e[0] for e in session.events]
            last_key=max(i for i,k in enumerate(kinds) if k=='key')
            self.assertEqual(kinds[last_key-2:last_key+1],['move','key','key'])
        finally:host.restore_local()
    def test_edge_signal_does_not_trigger_without_explicit_opt_in(self):
        backend=FakeBackend();host=X11Host(screen_id=1,target_size=(10,10),capture_center=(50,50),initial=(65535,0),backend_factory=lambda:backend,edge_directions=('right',))
        session=FakeSession();host.start(acknowledged=True,input_state_clear=True)
        try:
            backend.pending.extend([Event('motion',x=60,y=50),Event('motion',x=60,y=50)])
            time.sleep(.03);host.pump(session)
            edges=[fields for event,fields in session.events if event=='edge']
            self.assertEqual(len(edges),1);self.assertEqual(edges[0]['direction'],'right');self.assertTrue(edges[0]['clear'])
        finally:host.restore_local()

    def test_edge_reports_held_key_so_handoff_can_be_refused(self):
        backend=FakeBackend();host=X11Host(screen_id=1,target_size=(10,10),capture_center=(50,50),initial=(65535,0),backend_factory=lambda:backend,edge_directions=('right',))
        session=FakeSession();host.start(acknowledged=True,input_state_clear=True)
        try:
            backend.pending.extend([Event('key',code=38,down=True),Event('motion',x=60,y=50)])
            time.sleep(.03);host.pump(session)
            edges=[fields for event,fields in session.events if event=='edge']
            self.assertEqual(len(edges),1);self.assertFalse(edges[0]['clear'])
        finally:host.restore_local()
        backend=FakeBackend();host=X11Host(screen_id=1,target_size=(10,10),capture_center=(50,50),initial=(65535,0),backend_factory=lambda:backend)
        session=FakeSession();host.start(acknowledged=True,input_state_clear=True)
        try:
            backend.pending.append(Event('motion',x=60,y=50));time.sleep(.03);host.pump(session)
            self.assertFalse(any(event=='edge' for event,_ in session.events))
        finally:host.restore_local()
