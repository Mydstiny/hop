import base64
import hashlib
import ssl
import unittest
from types import SimpleNamespace
from hop_linux.pairing import PendingPairing, PairingError


def cert(raw):return b'-----BEGIN CERTIFICATE-----\n'+base64.b64encode(raw)+b'\n-----END CERTIFICATE-----\n'
def fingerprint(pem):return hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem.decode())).hexdigest().upper()

class Store:
    def __init__(self):self.saved=[]
    def store_peer(self,certificate,pin):self.saved.append((certificate,pin))
    def get_peer(self,pin):
        from hop_linux.tls import VerifiedPeer
        return next((VerifiedPeer(cert,saved) for cert,saved in self.saved if saved==pin.lower()),None)

class PairingTests(unittest.TestCase):
    def test_explicit_both_fingerprints_and_single_use(self):
        a,b=cert(b'a'),cert(b'b');store=Store();p=PendingPairing(a,b,fingerprint)
        self.assertEqual(store.saved,[])
        peer=p.confirm(fingerprint(a),fingerprint(b))
        self.assertEqual(peer.certificate,b);self.assertEqual(store.saved,[])
        session=SimpleNamespace(transport=SimpleNamespace(authenticated=True,local_fingerprint=fingerprint(a).lower(),peer_fingerprint=fingerprint(b).lower()),pairing_confirmed=True)
        p.commit(session,store);self.assertEqual(store.saved,[(b,fingerprint(b).lower())])
        with self.assertRaises(PairingError):p.confirm(fingerprint(a),fingerprint(b))
    def test_mismatch_expiry_and_cancel_do_not_trust(self):
        a,b=cert(b'a'),cert(b'b');store=Store();p=PendingPairing(a,b,fingerprint)
        with self.assertRaises(PairingError):p.confirm(fingerprint(a),'0'*64)
        with self.assertRaises(PairingError):p.confirm(fingerprint(a),fingerprint(b))
        now=[0];p=PendingPairing(a,b,fingerprint,now=lambda:now[0],timeout=1);now[0]=1
        with self.assertRaises(PairingError):p.confirm(fingerprint(a),fingerprint(b))
        p=PendingPairing(a,b,fingerprint);p.cancel()
        with self.assertRaises(PairingError):p.confirm(fingerprint(a),fingerprint(b))
        self.assertEqual(store.saved,[])
    def test_same_identity_and_false_calculator_rejected(self):
        a,b=cert(b'a'),cert(b'b')
        with self.assertRaises(ValueError):PendingPairing(a,a,fingerprint)
        with self.assertRaises(ValueError):PendingPairing(a,b,lambda c:'1'*64 if c==a else '2'*64)

class PairingSessionTests(unittest.TestCase):
    def test_both_peers_confirm_before_trust_and_input(self):
        from hop_linux import codec
        from hop_linux.session import Session,ProtocolError
        from test_session import Transport,Sink
        a,b=cert(b'a'),cert(b'b');left,right=Transport(),Transport();left.peer=right;right.peer=left
        left.local_fingerprint=fingerprint(a).lower();left.peer_fingerprint=fingerprint(b).lower();right.local_fingerprint=fingerprint(b).lower();right.peer_fingerprint=fingerprint(a).lower()
        host=Session(left,codec,Sink(),'host',capabilities=33)
        receiver=Session(right,codec,Sink(),'receiver',capabilities=1)
        host.start();receiver.poll();host.poll()
        self.assertEqual((host.phase,receiver.phase),('pairing','pairing'))
        with self.assertRaises(ProtocolError):receiver.announce_screens([(1,0,0,10,10)])
        lp=PendingPairing(a,b,fingerprint);rp=PendingPairing(b,a,fingerprint)
        lp.confirm(fingerprint(a),fingerprint(b));rp.confirm(fingerprint(b),fingerprint(a))
        store=Store()
        host.confirm_pairing(lp);receiver.poll()
        self.assertEqual(receiver.phase,'pairing');self.assertEqual(store.saved,[])
        receiver.confirm_pairing(rp);host.poll()
        self.assertTrue(host.pairing_confirmed and receiver.pairing_confirmed)
        lp.commit(host,store);rp.commit(receiver,store)
        self.assertEqual(len(store.saved),2)
        host.close();receiver.close()
    def test_no_commit_before_peer_ack_and_mismatch_clears_candidate(self):
        a,b=cert(b'a'),cert(b'b');p=PendingPairing(a,b,fingerprint);p.confirm(fingerprint(a),fingerprint(b));store=Store()
        session=SimpleNamespace(transport=SimpleNamespace(authenticated=True,local_fingerprint=fingerprint(a).lower(),peer_fingerprint=fingerprint(b).lower()),pairing_confirmed=False)
        with self.assertRaises(PairingError):p.commit(session,store)
        self.assertEqual(store.saved,[])
        with self.assertRaises(PairingError):p.verify_session(session.transport)
        p=PendingPairing(a,b,fingerprint);p.confirm(fingerprint(a),fingerprint(b))
        wrong_local=SimpleNamespace(authenticated=True,local_fingerprint='0'*64,peer_fingerprint=fingerprint(b).lower())
        with self.assertRaises(PairingError):p.verify_session(wrong_local)
