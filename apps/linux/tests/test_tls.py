"""Pinned TLS socket tests; real loopback is opt-in with generated fixtures."""
import gc
import hashlib
import os
import select
import socket
import ssl
import time
import unittest
from pathlib import Path

from hop_linux.tls import SecureSocket, TLSError, TLSLibrary, VerifiedPeer


def fingerprint(pem):
    return hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem.decode("ascii").strip())).hexdigest()


def tcp_pair():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0)); listener.listen(1)
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        client.connect(listener.getsockname())
        server, _ = listener.accept()
        return client, server
    finally:
        listener.close()


class TLSPureTests(unittest.TestCase):
    def test_library_path_must_be_explicit_absolute(self):
        with self.assertRaises(ValueError): TLSLibrary("libhop_tls_runtime.so")
    def test_peer_pin_and_certificate_shape_are_required(self):
        for cert, pin in ((b"", "0" * 64), (b"bad\0", "0" * 64), (b"bad", "0" * 64), (b"bad", "x" * 64)):
            with self.assertRaises(ValueError): VerifiedPeer(cert, pin)


@unittest.skipUnless(os.environ.get("HOP_TEST_TLS_LIBRARY") and os.environ.get("HOP_TEST_TLS_FIXTURE_DIR"),
                     "requires a locally built Hop TLS shared library and synthetic certificates")
class TLSLoopbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = TLSLibrary(os.environ["HOP_TEST_TLS_LIBRARY"])
        base = Path(os.environ["HOP_TEST_TLS_FIXTURE_DIR"])
        cls.certs = {name: (base / (name + ".crt")).read_bytes() for name in ("a", "b", "bad")}
        cls.keys = {name: (base / (name + ".key")).read_bytes() for name in ("a", "b", "bad")}

    def make_pair(self, pin_b="b", server_identity="b"):
        a, b = tcp_pair()
        try:
            client = SecureSocket(self.library, a, client=True, own_certificate=self.certs["a"],
                                  private_key=self.keys["a"], peer=VerifiedPeer(self.certs[pin_b], fingerprint(self.certs[pin_b])))
            try:
                server = SecureSocket(self.library, b, client=False, own_certificate=self.certs[server_identity],
                                      private_key=self.keys[server_identity], peer=VerifiedPeer(self.certs["a"], fingerprint(self.certs["a"])))
            except BaseException:
                client.close(); raise
            return client, server
        except BaseException:
            a.close(); b.close(); raise

    def handshake_pair(self, client, server):
        end = time.monotonic() + 10
        while time.monotonic() < end:
            if not client.authenticated: client.handshake_step()
            if not server.authenticated: server.handshake_step()
            if client.authenticated and server.authenticated: return
            time.sleep(.001)
        self.fail("loopback TLS handshake timed out")

    def test_bidirectional_bytes_and_timeout(self):
        client, server = self.make_pair()
        try:
            self.assertEqual(client._socket.getsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY), 1)
            self.assertEqual(server._socket.getsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY), 1)
            with self.assertRaises(TLSError): client.write_all(b"before-auth")
            self.handshake_pair(client, server)
            gc.collect()  # callback references remain alive for the entire native channel
            self.assertIsNone(server.read_some(timeout=.02))
            self.assertEqual(client.write_all(b"mac-to-linux"), 12)
            readable, _, _ = select.select([server._socket], [], [], 2)
            self.assertTrue(readable)
            encrypted_record = server._socket.recv(65536, socket.MSG_PEEK)
            self.assertNotIn(b"mac-to-linux", encrypted_record)
            self.assertEqual(server.read_some(), b"mac-to-linux")
            self.assertEqual(server.write_all(b"linux-to-mac"), 12)
            self.assertEqual(client.read_some(), b"linux-to-mac")
            self.assertTrue(client.authenticated and server.authenticated)
        finally:
            client.close(); server.close()

    def test_mismatched_preverified_certificate_never_authenticates(self):
        client, server = self.make_pair(pin_b="bad")
        try:
            with self.assertRaises(TLSError): self.handshake_pair(client, server)
            self.assertFalse(client.authenticated)
            with self.assertRaises(TLSError): client.write_all(b"must-not-send")
        finally:
            client.close(); server.close()

    def test_wrong_fingerprint_rejected_before_connection(self):
        with self.assertRaisesRegex(ValueError, "fingerprint"):
            VerifiedPeer(self.certs["b"], "0" * 64)
        with self.assertRaises(ValueError):
            VerifiedPeer(self.certs["b"] + self.certs["bad"], fingerprint(self.certs["b"]))
        peer = VerifiedPeer(self.certs["b"], fingerprint(self.certs["b"]))
        with self.assertRaises(AttributeError): peer.certificate = self.certs["bad"]

    def test_handshake_timeout_closes_channel(self):
        client, server = self.make_pair()
        try:
            with self.assertRaisesRegex(TLSError, "timed out"): client.handshake(timeout=.05)
            self.assertFalse(client.authenticated)
            with self.assertRaises(TLSError): client.write_all(b"after-timeout")
        finally:
            client.close(); server.close()

    def test_disconnect_fails_closed(self):
        client, server = self.make_pair()
        try:
            self.handshake_pair(client, server)
            server.close()
            with self.assertRaises((EOFError, TLSError)):
                client.read_some(timeout=3)
            self.assertFalse(client.authenticated)
            with self.assertRaises(TLSError): client.write_all(b"after-disconnect")
        finally:
            client.close(); server.close()

    def test_authenticated_session_focus_input_and_disconnect(self):
        from hop_linux.session import Session
        from test_session import Codec, Sink
        client, server = self.make_pair()
        host = receiver = None
        try:
            self.handshake_pair(client, server)
            host = Session(client, Codec, Sink(), 'host', capabilities=1)
            receiver = Session(server, Codec, Sink(), 'receiver', capabilities=1)
            host.start(); receiver.poll(2); host.poll(2)
            receiver.announce_screens([(1,0,0,1920,1080)]); host.poll(2)
            host.enter(1); receiver.poll(2); host.poll(2)
            self.assertEqual(host.phase, 'remote')
            host.send_input('key', usage=4, modifiers=0, down=1, repeat=0); receiver.poll(2)
            self.assertEqual(receiver.sink.events[0][0], 'key')
            host.close()
            with self.assertRaises((EOFError, TLSError)): receiver.poll(2)
            self.assertTrue(receiver.closed)
            self.assertGreaterEqual(receiver.sink.releases, 2)
            self.assertGreaterEqual(receiver.sink.restores, 1)
        finally:
            if host: host.close()
            else: client.close()
            if receiver: receiver.close()
            else: server.close()

    def test_mutual_confirmation_over_authenticated_tls_before_persisting(self):
        from hop_linux import codec
        from hop_linux.pairing import PendingPairing, PairingError
        from hop_linux.session import Session
        from test_pairing import Store
        from test_session import Sink
        client,server=self.make_pair();host=receiver=None
        try:
            self.handshake_pair(client,server)
            host=Session(client,codec,Sink(),'host',capabilities=33)
            receiver=Session(server,codec,Sink(),'receiver',capabilities=1)
            host.start();receiver.poll(2);host.poll(2)
            local=PendingPairing(self.certs['a'],self.certs['b'],fingerprint)
            remote=PendingPairing(self.certs['b'],self.certs['a'],fingerprint)
            local.confirm(fingerprint(self.certs['a']),fingerprint(self.certs['b']))
            remote.confirm(fingerprint(self.certs['b']),fingerprint(self.certs['a']))
            left,right=Store(),Store()
            host.confirm_pairing(local);receiver.poll(2)
            self.assertEqual(left.saved,[]);self.assertEqual(right.saved,[])
            receiver.confirm_pairing(remote);host.poll(2)
            local.commit(host,left);remote.commit(receiver,right)
            self.assertEqual(len(left.saved),1);self.assertEqual(len(right.saved),1)
        finally:
            if host:host.close()
            else:client.close()
            if receiver:receiver.close()
            else:server.close()


if __name__ == "__main__": unittest.main()
