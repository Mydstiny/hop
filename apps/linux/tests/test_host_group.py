import unittest
from types import SimpleNamespace
from hop_linux.host_group import FocusAuthority,HostGroup,HostGroupError


def session(pin):return SimpleNamespace(transport=SimpleNamespace(peer_fingerprint=pin,authenticated=True),closed=False)


class FocusAuthorityTests(unittest.TestCase):
    def setUp(self):
        self.group=FocusAuthority();self.a=object();self.b=object();self.sa=session('a');self.sb=session('b')
        self.group.register(self.a,self.sa);self.group.register(self.b,self.sb)
    def test_reservation_waits_for_old_release_and_excludes_other_sessions(self):
        self.group.claim(self.a,self.sa)
        with self.assertRaises(HostGroupError):self.group.claim(self.b,self.sb)
        self.assertTrue(self.group.reserve(self.a,self.sa,'b','route'))
        with self.assertRaises(HostGroupError):self.group.claim(self.b,self.sb)
        self.assertEqual(self.group.released(self.a,self.sa),(self.b,self.sb,'route'))
        self.group.claim(self.b,self.sb)
        with self.assertRaises(HostGroupError):self.group.claim(self.a,self.sa)
    def test_disconnected_or_replaced_target_cannot_acquire(self):
        self.group.claim(self.a,self.sa);self.assertTrue(self.group.reserve(self.a,self.sa,'b','route'))
        self.sb.closed=True;self.group.unregister(self.b,self.sb)
        new=session('b');self.group.register(self.b,new)
        self.assertIsNone(self.group.released(self.a,self.sa))
        self.group.claim(self.b,new)
    def test_failure_clears_reservation_without_entering_target(self):
        self.group.claim(self.a,self.sa);self.assertTrue(self.group.reserve(self.a,self.sa,'b','route'))
        self.group.unregister(self.a,self.sa)
        with self.assertRaises(HostGroupError):self.group.released(self.a,self.sa)
        self.group.claim(self.b,self.sb)
    def test_duplicate_certificates_are_rejected(self):
        with self.assertRaises(HostGroupError):self.group.register(object(),session('a'))


class HostLayoutGroupTests(unittest.TestCase):
    def test_connected_devices_share_and_update_one_layout(self):
        from hop_linux.layout import Device,Screen
        class Controller:
            def __init__(self):self.events=[];self.layout=None
            def _event(self,event):self.events.append(event)
        local_id='0'*64;a='a'*64;b='b'*64
        local=Device(local_id,'local','linux',0,0,(Screen('1',0,0,100,100),))
        da=Device(a,'a','macos',100,0,(Screen('7',0,0,100,100),))
        db=Device(b,'b','windows',100,0,(Screen('8',0,0,100,100),))
        ca,cb=Controller(),Controller();sa,sb=session(a),session(b);group=HostGroup()
        group.register(ca,sa);group.update_screens(ca,sa,local,da)
        group.register(cb,sb);group.update_screens(cb,sb,local,db)
        self.assertIs(ca.layout,cb.layout);self.assertEqual(len(group.layout.devices),3)
        self.assertFalse(group.layout.validate().errors)
        moved=group.layout.move(b,200,100);group.set_layout(moved)
        self.assertEqual(ca.layout.devices[-1].y,100)
        group.authority.claim(ca,sa)
        with self.assertRaises(HostGroupError):group.set_layout(group.layout)
        group.authority.abort(ca,sa);group.unregister(cb,sb)
        self.assertEqual(len(ca.layout.devices),2);group.unregister(ca,sa)
        self.assertIsNone(group.layout)
