import unittest
from hop_linux.x11_sink import X11Sink

class Backend:
    def __init__(self): self.events=[]
    def input_state_clear(self):return True
    def motion(self,*a): self.events.append(('move',*a))
    def button(self,*a): self.events.append(('button',*a))
    def key_hid(self,*a): self.events.append(('key',*a))
    def scroll(self,*a): self.events.append(('scroll',*a))
    def release_all(self): self.events.append(('release',));return []

class SinkTests(unittest.TestCase):
    def test_screen_scaling_buttons_and_fractional_wheel(self):
        b=Backend();s=X11Sink(b,None,[(3,100,200,801,601)])
        s.enter(dict(screen_id=3,x=65535,y=0,modifiers=0,buttons=0))
        self.assertEqual(b.events[-1],('move',900,200))
        s.inject('button',dict(screen_id=3,x=0,y=65535,button=2,down=1))
        self.assertEqual(b.events[-2:], [('move',100,800),('button',3,True)])
        s.inject('wheel',dict(horizontal=60,vertical=0));s.inject('wheel',dict(horizontal=60,vertical=-120))
        self.assertEqual(b.events[-1],('scroll',1,-1))
        s.release_all()
        with self.assertRaises(ValueError): s.inject('wheel',dict(horizontal=120,vertical=0))
    def test_unsafe_geometry_and_held_entry_rejected(self):
        with self.assertRaises(ValueError): X11Sink(Backend(),None,[(1,-1,0,100,100)])
        s=X11Sink(Backend(),None,[(1,0,0,100,100)])
        with self.assertRaises(ValueError): s.enter(dict(screen_id=1,x=0,y=0,modifiers=1,buttons=0))
