import unittest
import os
import time
from pathlib import Path
from hop_linux.multi_session import MultiSessionHost,MultiSessionError


class Transport:
    authenticated=True
    def __init__(self,peer):self.peer_fingerprint=peer


class Session:
    role='host'
    def __init__(self,peer,events):
        self.transport=Transport(peer);self.peer=peer;self.events=events;self.phase='local';self.closed=False
        self.screens=((1,0,0,100,100),(2,100,0,100,100));self.sink=None;self.screen_id=0
    def enter(self,screen,x,y):self.events.append(('enter',self.peer,screen,x,y));self.screen_id=screen;self.phase='entering'
    def release(self):self.events.append(('release',self.peer));self.sink.restore_local();self.phase='releasing'
    def poll(self,_timeout):pass
    def close(self):self.closed=True;self.transport.authenticated=False


class Capture:
    def __init__(self,peer,events):self.peer=peer;self.events=events
    def start(self,session):self.events.append(('capture',self.peer))
    def pump(self,session):pass
    def restore_local(self):self.events.append(('restore',self.peer))
    def release_all(self):pass


class MultiSessionTests(unittest.TestCase):
    def setUp(self):
        self.events=[];self.a=Session('a',self.events);self.b=Session('b',self.events)
        self.clear=True;self.now=0
        self.router=MultiSessionHost({'a':self.a,'b':self.b},lambda peer,*_:Capture(peer,self.events),lambda:self.clear,clock=lambda:self.now)
    def tearDown(self):self.router.close()
    def activate_a(self):
        self.router.switch('a',1,0,42);self.assertEqual(self.a.phase,'entering')
        self.a.phase='remote';self.router.tick();self.assertEqual(self.router.active,'a')
    def test_three_device_switch_waits_for_release_ack_before_enter(self):
        self.activate_a();self.router.switch('b',1,0,30)
        self.assertEqual(self.events[-2:],[('release','a'),('restore','a')]);self.router.tick()
        self.assertEqual(self.b.phase,'local');self.assertEqual(self.router.phase,'releasing')
        self.a.phase='local';self.router.tick();self.assertEqual(self.b.phase,'entering')
        self.assertEqual(self.events[-1],('enter','b',1,0,30))
        self.b.phase='remote';self.router.tick();self.assertEqual(self.router.active,'b')
        self.assertEqual(self.events[-1],('capture','b'))
    def test_held_key_and_disconnect_fail_safe(self):
        self.clear=False
        with self.assertRaises(MultiSessionError):self.router.switch('a',1,0,0)
        self.assertFalse(self.events)
        self.clear=True;self.activate_a();self.router.switch('b',1,0,0)
        self.b.transport.authenticated=False
        with self.assertRaises(MultiSessionError):self.router.tick()
        self.assertTrue(self.router.closed);self.assertNotIn(('enter','b',1,0,0),self.events)
    def test_state_changed_during_ack_wait_aborts(self):
        self.activate_a();self.router.switch('b',1,0,0);self.a.phase='local';self.clear=False
        with self.assertRaises(MultiSessionError):self.router.tick()
        self.assertTrue(self.router.closed);self.assertEqual(self.b.phase,'local')
    def test_switch_to_other_display_on_same_device(self):
        self.activate_a();self.router.switch('a',2,0,20)
        self.assertEqual(self.a.phase,'releasing');self.a.phase='local';self.router.tick()
        self.assertEqual(self.a.phase,'entering');self.assertEqual(self.events[-1],('enter','a',2,0,20))
        self.a.phase='remote';self.router.tick();self.assertEqual(self.router.active,'a')

    def test_layout_routes_across_three_devices_and_back(self):
        from hop_linux.layout import Device,Layout,Screen
        local,a,b=('0'*64,'a'*64,'b'*64)
        layout=Layout(tuple(Device(ident,ident[:1],'linux',x,0,(Screen('1',0,0,100,100),)) for ident,x in ((local,0),(a,100),(b,200))))
        first,second=Session(a,self.events),Session(b,self.events);restored=[]
        router=MultiSessionHost({a:first,b:second},lambda peer,*_:Capture(peer,self.events),lambda:True)
        try:
            self.assertTrue(router.route_edge(layout,local,local,'1','right',65535,32000,restored.append))
            first.phase='remote';router.tick();self.assertEqual(router.active,a)
            self.assertTrue(router.route_edge(layout,local,a,'1','right',65535,32000,restored.append))
            router.tick();self.assertEqual(second.phase,'local')
            first.phase='local';router.tick();self.assertEqual(second.phase,'entering')
            second.phase='remote';router.tick();self.assertEqual(router.active,b)
            self.assertTrue(router.route_edge(layout,local,b,'1','left',0,32000,restored.append))
            second.phase='local';router.tick();first.phase='remote';router.tick()
            self.assertTrue(router.route_edge(layout,local,a,'1','left',0,32000,restored.append))
            self.assertFalse(restored);first.phase='local';router.tick()
            self.assertEqual(restored[0].device_id,local);self.assertEqual(router.phase,'local')
        finally:router.close()
    def test_mismatched_pin_rejected(self):
        with self.assertRaises(ValueError):MultiSessionHost({'wrong':self.a},lambda *_:None,lambda:True)

    def test_real_v2_sessions_wait_for_receiver_barrier(self):
        from test_session import Sink,Transport as TestTransport
        from hop_linux.session import Session as WireSession
        from hop_linux import codec
        class Barrier:
            supported_capabilities=2
            def __init__(self):self.ready=True
            def focus(self,epoch):pass
            def barrier_ready(self):return self.ready
            def expect_initial_snapshot(self):pass
            def initial_done(self):pass
            def initial_processed(self):return True
            def close(self):pass
        hosts={};receivers=[];barriers=[]
        for peer in ('x','y'):
            a,b=TestTransport(),TestTransport();a.peer=b;b.peer=a;a.peer_fingerprint=peer
            barrier=Barrier();barriers.append(barrier)
            h=WireSession(a,codec,Sink(),'host',capabilities=3,clipboard=Barrier())
            r=WireSession(b,codec,Sink(),'receiver',capabilities=3,clipboard=barrier)
            h.start();r.poll();h.poll();r.announce_screens([(1,0,0,100,100)]);h.poll()
            hosts[peer]=h;receivers.append(r)
        router=MultiSessionHost(hosts,lambda peer,*_:Capture(peer,self.events),lambda:True)
        try:
            router.switch('x',1,0,100);receivers[0].poll();router.tick();self.assertEqual(router.active,'x')
            barriers[0].ready=False;router.switch('y',1,0,200);receivers[0].poll();router.tick()
            self.assertEqual(hosts['y'].phase,'local')
            barriers[0].ready=True;receivers[0].poll();router.tick();self.assertEqual(hosts['y'].phase,'entering')
            receivers[1].poll();router.tick();self.assertEqual(router.active,'y')
        finally:
            router.close()
            for receiver in receivers:receiver.close()


@unittest.skipUnless(os.environ.get('HOP_TEST_TLS_LIBRARY') and os.environ.get('HOP_TEST_TLS_FIXTURE_DIR'),
                     'requires native TLS runtime and synthetic certificates')
class MultiSessionTLSTests(unittest.TestCase):
    def test_two_pinned_tls_sessions_switch_after_release_ack(self):
        from test_tls import tcp_pair,fingerprint
        from test_session import Sink
        from hop_linux.tls import SecureSocket,TLSLibrary,VerifiedPeer
        from hop_linux.session import Session as WireSession
        from hop_linux import codec
        base=Path(os.environ['HOP_TEST_TLS_FIXTURE_DIR']);lib=TLSLibrary(os.environ['HOP_TEST_TLS_LIBRARY'])
        cert=lambda n:(base/(n+'.crt')).read_bytes()
        key=lambda n:(base/(n+'.key')).read_bytes()
        hosts={};receivers=[];channels=[];events=[];router=None
        try:
            for name in ('b','bad'):
                a,b=tcp_pair()
                c=SecureSocket(lib,a,client=True,own_certificate=cert('a'),private_key=key('a'),peer=VerifiedPeer(cert(name),fingerprint(cert(name))))
                s=SecureSocket(lib,b,client=False,own_certificate=cert(name),private_key=key(name),peer=VerifiedPeer(cert('a'),fingerprint(cert('a'))))
                channels.extend((c,s));deadline=time.monotonic()+8
                while not (c.authenticated and s.authenticated) and time.monotonic()<deadline:
                    if not c.authenticated:c.handshake_step()
                    if not s.authenticated:s.handshake_step()
                    time.sleep(.001)
                self.assertTrue(c.authenticated and s.authenticated)
                h=WireSession(c,codec,Sink(),'host',capabilities=1);r=WireSession(s,codec,Sink(),'receiver',capabilities=1)
                h.start();r.poll(2);h.poll(2);r.announce_screens([(1,0,0,100,100)]);h.poll(2)
                hosts[c.peer_fingerprint]=h;receivers.append(r)
            peers=list(hosts);router=MultiSessionHost(hosts,lambda peer,*_:Capture(peer,events),lambda:True)
            router.switch(peers[0],1,0,200);receivers[0].poll(2)
            deadline=time.monotonic()+3
            while router.active!=peers[0] and time.monotonic()<deadline:router.tick(.01)
            self.assertEqual(router.active,peers[0]);router.switch(peers[1],1,0,300)
            self.assertEqual(hosts[peers[1]].phase,'local');receivers[0].poll(2)
            while hosts[peers[1]].phase!='entering' and time.monotonic()<deadline:router.tick(.01)
            self.assertEqual(hosts[peers[1]].phase,'entering');receivers[1].poll(2)
            while router.active!=peers[1] and time.monotonic()<deadline:router.tick(.01)
            self.assertEqual(router.active,peers[1]);self.assertEqual([e[0] for e in events],['capture','restore','capture'])
        finally:
            if router:router.close()
            for receiver in receivers:receiver.close()
            for channel in channels:channel.close()
