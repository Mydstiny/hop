import os
from pathlib import Path
import socket
import threading
import unittest
from hop_linux.reconnect import reconnect_verified_socket
from hop_linux.tls import TLSLibrary,VerifiedPeer
from test_tls import fingerprint
from test_session import Sink

class Store:
    def __init__(self,*certs):self.peers={fingerprint(c):VerifiedPeer(c,fingerprint(c)) for c in certs}
    def get_peer(self,pin):return self.peers.get(pin.lower()) if isinstance(pin,str) else None

@unittest.skipUnless(os.environ.get('HOP_TEST_TLS_LIBRARY') and os.environ.get('HOP_TEST_TLS_FIXTURE_DIR'),'requires native TLS fixture')
class ReconnectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib=TLSLibrary(os.environ['HOP_TEST_TLS_LIBRARY']);base=Path(os.environ['HOP_TEST_TLS_FIXTURE_DIR'])
        cls.certs={n:(base/(n+'.crt')).read_bytes() for n in ('a','b','bad')};cls.keys={n:(base/(n+'.key')).read_bytes() for n in ('a','b','bad')}
    def test_reconnect_both_pins_and_unknown_hint(self):
        left,right=socket.socketpair();results=[];errors=[];barrier=threading.Barrier(2)
        def worker(sock,own,other,role,initiator):
            session=None
            try:
                session=reconnect_verified_socket(sock,library=self.lib,own_certificate=self.certs[own],private_key=self.keys[own],role=role,tcp_initiator=initiator,sink=Sink(),trust_store=Store(self.certs[other]),peer_fingerprint=fingerprint(self.certs[other]))
                results.append((role,session.phase));barrier.wait(3)
            except Exception as exc:errors.append(exc)
            finally:
                if session:session.close()
        ts=[threading.Thread(target=worker,args=(left,'a','b','host',True)),threading.Thread(target=worker,args=(right,'b','a','receiver',False))]
        for t in ts:t.start()
        for t in ts:t.join(8)
        self.assertFalse(errors);self.assertEqual(sorted(results),[('host','local'),('receiver','local')])
        a,b=socket.socketpair()
        try:
            from hop_linux.pair_offer import encode_selector
            a.sendall(encode_selector('A'*64))
            with self.assertRaises(Exception):reconnect_verified_socket(b,library=self.lib,own_certificate=self.certs['b'],private_key=self.keys['b'],role='receiver',tcp_initiator=False,sink=Sink(),trust_store=Store(self.certs['a']))
            self.assertEqual(b.fileno(),-1)
        finally:a.close();b.close()
    def test_peer_still_requiring_pairing_is_not_silently_trusted(self):
        from hop_linux import codec
        from hop_linux.pair_offer import accept_selected
        from hop_linux.session import Session
        left,right=socket.socketpair();errors=[]
        def peer():
            session=None
            try:
                channel=accept_selected(right,library=self.lib,own_certificate=self.certs['b'],private_key=self.keys['b'],lookup_verified_peer=Store(self.certs['a']).get_peer)
                session=Session(channel,codec,Sink(),'receiver',capabilities=33)
                session.poll(3)
            except Exception as exc:errors.append(exc)
            finally:
                if session:session.close()
        t=threading.Thread(target=peer);t.start()
        with self.assertRaisesRegex(Exception,'explicit fingerprint confirmation'):
            reconnect_verified_socket(left,library=self.lib,own_certificate=self.certs['a'],private_key=self.keys['a'],role='host',tcp_initiator=True,sink=Sink(),trust_store=Store(self.certs['b']),peer_fingerprint=fingerprint(self.certs['b']))
        t.join(5);self.assertFalse(t.is_alive());self.assertFalse(errors)
