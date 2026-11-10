import json
import os
import platform
from pathlib import Path
import socket
import threading
import time
import unittest
from hop_linux.pair_offer import Candidate,CandidateWindow,OfferError,decode_offer,encode_offer,encode_selector,decode_selector,exchange_candidate,read_selector
from test_pairing import cert,fingerprint

class FakeTLS:
    def random_bytes(self,size):return os.urandom(size)
    def certificate_fingerprint(self,certificate):return fingerprint(certificate)

class OfferTests(unittest.TestCase):
    def test_explicit_two_way_exchange_is_untrusted_and_closes_socket(self):
        a,b=socket.socketpair();results=[];errors=[]
        def worker(s,role):
            try:results.append(exchange_candidate(s,own_role=role,name='测试',certificate=cert(bytes([role])),tls_library=FakeTLS(),explicitly_started=True))
            except Exception as exc:errors.append(exc)
        threads=[threading.Thread(target=worker,args=(a,1)),threading.Thread(target=worker,args=(b,2))]
        for t in threads:t.start()
        for t in threads:t.join(3)
        self.assertFalse(errors);self.assertEqual(sorted(c.role for c in results),[1,2]);self.assertEqual(a.fileno(),-1);self.assertEqual(b.fileno(),-1)
    def test_invalid_names_nonce_boundaries_and_extra_frame(self):
        valid=Candidate(1,'ok',cert(b'a'),b'1'*16);data=encode_offer(valid,fingerprint)
        self.assertEqual(decode_offer(data,fingerprint),valid)
        for bad in (data+b'x',data[:-1]):
            with self.assertRaises(OfferError):decode_offer(bad,fingerprint)
        for c in (Candidate(1,'bad\u202e',valid.certificate,valid.nonce),Candidate(1,'x'*129,valid.certificate,valid.nonce),Candidate(1,'ok',valid.certificate,b'\0'*16)):
            with self.assertRaises(OfferError):encode_offer(c,fingerprint)
    def test_responder_waits_for_half_close_and_rejects_delayed_tail(self):
        a,b=socket.socketpair();errors=[]
        def server():
            try:exchange_candidate(b,own_role=2,name='peer',certificate=cert(b'b'),tls_library=FakeTLS(),explicitly_started=True,timeout=2)
            except Exception as exc:errors.append(exc)
        t=threading.Thread(target=server);t.start()
        try:
            data=encode_offer(Candidate(1,'host',cert(b'a'),b'1'*16),fingerprint)
            a.sendall(data);a.settimeout(.05)
            with self.assertRaises(socket.timeout):a.recv(1)
            a.sendall(b'x');a.shutdown(socket.SHUT_WR);a.settimeout(2)
            self.assertEqual(a.recv(1),b'');t.join(2)
            self.assertEqual(len(errors),1);self.assertIsInstance(errors[0],OfferError)
        finally:a.close();t.join(2)
    def test_responder_rejects_truncated_request_without_response(self):
        a,b=socket.socketpair();errors=[]
        def server():
            try:exchange_candidate(b,own_role=2,name='peer',certificate=cert(b'b'),tls_library=FakeTLS(),explicitly_started=True,timeout=2)
            except Exception as exc:errors.append(exc)
        t=threading.Thread(target=server);t.start()
        try:
            data=encode_offer(Candidate(1,'host',cert(b'a'),b'1'*16),fingerprint)
            a.sendall(data[:-1]);a.shutdown(socket.SHUT_WR);a.settimeout(2)
            self.assertEqual(a.recv(1),b'');t.join(2)
            self.assertEqual(len(errors),1);self.assertIsInstance(errors[0],OfferError)
        finally:a.close();t.join(2)
    def test_selector_does_not_consume_queued_tls_bytes(self):
        a,b=socket.socketpair()
        try:
            a.sendall(encode_selector('A'*64)+b'tls-bytes')
            self.assertEqual(read_selector(b),'A'*64);self.assertEqual(b.recv(9),b'tls-bytes')
            with self.assertRaises(OfferError):encode_selector('a'*64)
        finally:a.close();b.close()
    def test_offer_timeout_and_explicit_activation(self):
        a,b=socket.socketpair()
        try:
            with self.assertRaises(OfferError):exchange_candidate(a,own_role=1,name='x',certificate=cert(b'a'),tls_library=FakeTLS(),explicitly_started=False)
            with self.assertRaises(OfferError):exchange_candidate(a,own_role=1,name='x',certificate=cert(b'a'),tls_library=FakeTLS(),explicitly_started=True,timeout=.05)
            self.assertEqual(a.fileno(),-1)
        finally:a.close();b.close()
    @unittest.skipUnless(platform.system()=='Linux','Linux listener validation')
    def test_window_one_candidate_and_serial_bounds(self):
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(2)
        window=CandidateWindow(listener,own_role=2,name='peer',certificate=cert(b'b'),tls_library=FakeTLS(),explicitly_started=True,lifetime=2)
        output=[];errors=[]
        def server():
            try:output.append(window.accept_once())
            except Exception as exc:errors.append(exc)
        t=threading.Thread(target=server);t.start()
        try:
            deadline=time.monotonic()+1
            while not window.lock.locked() and time.monotonic()<deadline:time.sleep(.001)
            self.assertTrue(window.lock.locked())
            with self.assertRaises(OfferError):window.accept_once()
            client=socket.socket();client.connect(listener.getsockname())
            candidate=exchange_candidate(client,own_role=1,name='host',certificate=cert(b'a'),tls_library=FakeTLS(),explicitly_started=True)
            t.join(3)
            self.assertFalse(errors);self.assertEqual(candidate.role,2);self.assertEqual(output[0].role,1);self.assertTrue(window.closed)
            with self.assertRaises(OfferError):window.accept_once()
        finally:window.close();t.join(3)

VECTORS=Path(os.environ.get('HOP_PAIRING_VECTORS',Path(__file__).resolve().parents[3]/'protocol/v2-pairing-vectors.json'))
@unittest.skipUnless(VECTORS.is_file(),'requires shared pairing vectors')
class OfferVectorTests(unittest.TestCase):
    def test_shared_vectors(self):
        for v in json.loads(VECTORS.read_text()):
            with self.subTest(name=v['name']):
                data=bytes.fromhex(v['hex']);fn=(lambda:decode_offer(data,fingerprint)) if v['type']=='offer' else (lambda:decode_selector(data))
                if v['valid']:fn()
                else:
                    with self.assertRaises(OfferError):fn()

@unittest.skipUnless(os.environ.get('HOP_TEST_TLS_LIBRARY') and os.environ.get('HOP_TEST_TLS_FIXTURE_DIR'),'requires synthetic TLS fixtures')
class SecureOfferTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from hop_linux.tls import TLSLibrary
        cls.lib=TLSLibrary(os.environ['HOP_TEST_TLS_LIBRARY']);base=Path(os.environ['HOP_TEST_TLS_FIXTURE_DIR'])
        cls.certs={n:(base/(n+'.crt')).read_bytes() for n in ('a','b')};cls.keys={n:(base/(n+'.key')).read_bytes() for n in ('a','b')}
    def test_real_native_identity_fingerprint_and_random(self):
        a,b=self.certs['a'],self.certs['b']
        self.assertTrue(self.lib.validate_identity(a,self.keys['a']))
        self.assertFalse(self.lib.validate_identity(a,self.keys['b']))
        self.assertEqual(self.lib.certificate_fingerprint(a),fingerprint(a))
        self.assertEqual(len(self.lib.random_bytes(16)),16)
        with self.assertRaises(ValueError):self.lib.certificate_fingerprint(b'bad')
        generated_cert,generated_key=self.lib.generate_identity()
        self.assertTrue(self.lib.validate_identity(generated_cert,generated_key))
        self.assertNotEqual(self.lib.certificate_fingerprint(generated_cert),self.lib.certificate_fingerprint(a))
    def test_real_offer_then_selector_and_pinned_tls(self):
        from hop_linux.pair_offer import connect_selected,accept_selected
        from hop_linux.tls import VerifiedPeer
        a,b=socket.socketpair();offers=[];errors=[]
        def offer(sock,role,name):
            try:offers.append(exchange_candidate(sock,own_role=role,name=name,certificate=self.certs[name],tls_library=self.lib,explicitly_started=True))
            except Exception as exc:errors.append(exc)
        ts=[threading.Thread(target=offer,args=(a,1,'a')),threading.Thread(target=offer,args=(b,2,'b'))]
        for t in ts:t.start()
        for t in ts:t.join(5)
        self.assertFalse(errors);self.assertEqual(len(offers),2)
        # Explicit out-of-band fingerprint comparison occurs before these pins.
        a,b=socket.socketpair();output=[]
        def server():
            try:
                def lookup(hint):
                    return VerifiedPeer(self.certs['a'],hint) if hint==fingerprint(self.certs['a']) else None
                with accept_selected(b,library=self.lib,own_certificate=self.certs['b'],private_key=self.keys['b'],lookup_verified_peer=lookup,timeout=5) as s:
                    output.append(s.read_some(timeout=3))
            except Exception as exc:errors.append(exc)
        t=threading.Thread(target=server);t.start()
        with connect_selected(a,library=self.lib,own_certificate=self.certs['a'],private_key=self.keys['a'],peer=VerifiedPeer(self.certs['b'],fingerprint(self.certs['b'])),timeout=5) as c:c.write_all(b'test')
        t.join(5);self.assertFalse(t.is_alive());self.assertFalse(errors);self.assertEqual(output,[b'test'])
    def test_unknown_hint_rejected_and_socket_closed(self):
        from hop_linux.pair_offer import accept_selected
        a,b=socket.socketpair()
        try:
            a.sendall(encode_selector('A'*64))
            with self.assertRaises(OfferError):accept_selected(b,library=self.lib,own_certificate=self.certs['b'],private_key=self.keys['b'],lookup_verified_peer=lambda _:None)
            self.assertEqual(b.fileno(),-1)
        finally:a.close();b.close()

    def test_verified_pairing_workflow_on_worker_threads(self):
        from hop_linux.pair_flow import pair_verified_socket
        from hop_linux.pairing import PendingPairing
        from test_pairing import Store
        from test_session import Sink
        left,right=socket.socketpair();stores=[Store(),Store()];results=[];errors=[]
        def worker(sock,own,other,role,initiator,index):
            try:
                pending=PendingPairing(self.certs[own],self.certs[other],self.lib.certificate_fingerprint)
                pending.confirm(fingerprint(self.certs[own]),fingerprint(self.certs[other]))
                session=pair_verified_socket(sock,pending=pending,library=self.lib,own_certificate=self.certs[own],private_key=self.keys[own],role=role,tcp_initiator=initiator,sink=Sink(),trust_store=stores[index])
                results.append((role,session.pairing_confirmed));session.close()
            except Exception as exc:errors.append(exc)
        threads=[threading.Thread(target=worker,args=(left,'a','b','host',True,0)),threading.Thread(target=worker,args=(right,'b','a','receiver',False,1))]
        for t in threads:t.start()
        for t in threads:t.join(20)
        self.assertFalse(any(t.is_alive() for t in threads));self.assertFalse(errors);self.assertEqual(len(results),2)
        self.assertTrue(all(ok for _,ok in results));self.assertEqual([len(s.saved) for s in stores],[1,1])

    def test_manual_tcp_endpoint_real_pairing_without_discovery(self):
        from hop_linux.controller import PairingController
        from hop_linux.tls import TLSError
        from hop_linux.pairing import PendingPairing
        from hop_linux.pair_flow import pair_verified_socket
        from test_pairing import Store
        from test_session import Sink
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(3);listener.settimeout(8)
        port=listener.getsockname()[1];approved=threading.Event();done=threading.Event();errors=[];remote_store=Store()
        def receiver():
            session=None
            try:
                conn,_=listener.accept();offer=exchange_candidate(conn,own_role=2,name='receiver',certificate=self.certs['b'],tls_library=self.lib,explicitly_started=True)
                pending=PendingPairing(self.certs['b'],offer.certificate,self.lib.certificate_fingerprint)
                if not approved.wait(5):raise RuntimeError('test confirmation timeout')
                pending.confirm(pending.local_fingerprint,pending.peer_fingerprint)
                conn,_=listener.accept();session=pair_verified_socket(conn,pending=pending,library=self.lib,own_certificate=self.certs['b'],private_key=self.keys['b'],role='receiver',tcp_initiator=False,sink=Sink(),trust_store=remote_store)
                while not done.is_set():
                    try:session.poll(.02)
                    except (EOFError,TLSError):break
            except Exception as exc:errors.append(exc)
            finally:
                if session:session.close()
                listener.close()
        thread=threading.Thread(target=receiver);thread.start();store=Store()
        controller=PairingController(name='host',certificate=self.certs['a'],private_key=self.keys['a'],tls_library=self.lib,trust_store=store)
        try:
            controller.begin_manual_pairing('127.0.0.1',port);deadline=time.monotonic()+8
            while controller.state!='comparison' and time.monotonic()<deadline:
                item=controller.poll()
                if item:controller.accept_event(item)
                time.sleep(.01)
            self.assertEqual(controller.state,'comparison');controller.confirm(True);approved.set()
            while controller.state!='connected' and time.monotonic()<deadline:
                item=controller.poll()
                if item:controller.accept_event(item)
                time.sleep(.01)
            self.assertEqual(controller.state,'connected');self.assertEqual(len(store.saved),1)
        finally:
            controller.close();done.set();thread.join(5)
            if controller.worker:controller.worker.join(5)
        self.assertFalse(thread.is_alive());self.assertFalse(errors)
