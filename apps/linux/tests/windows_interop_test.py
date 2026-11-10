"""Pair a Linux adapter with an explicitly isolated native Windows peer.

Uses only disposable identities and a synthetic input sink. The TCP initiator
and v2 input role are independent. Run with --isolated-interop after preparing
the Windows companion fixture and setting HOP_TEST_TLS_LIBRARY and
HOP_TEST_SHORTCUT_LIBRARY.
"""
import argparse
import ipaddress
import os
from pathlib import Path
import socket
import time

from hop_linux.pair_flow import pair_verified_socket
from hop_linux.pair_offer import exchange_candidate
from hop_linux.pairing import PendingPairing
from hop_linux.shortcut import ShortcutTranslator
from hop_linux.tls import TLSLibrary, VerifiedPeer


CAPABILITIES = 1 | 8 | 16
SCREEN = (7, 0, 0, 1280, 900)
COPY_KEYS = [(0xe0,1), (6,1), (6,0), (0xe0,0)]


class SyntheticSink:
    def __init__(self):
        self.entered = None
        self.events = []
        self.releases = 0
        self.restored = False

    def enter(self, fields):
        self.entered = fields['screen_id']

    def inject(self, event, fields):
        self.events.append((event, fields))

    def release_all(self):
        self.releases += 1

    def restore_local(self):
        self.restored = True


class SyntheticTrust:
    def __init__(self, expected):
        self.expected = expected
        self.peer = None

    def store_peer(self, certificate, fingerprint):
        assert VerifiedPeer(certificate, fingerprint) == self.expected
        self.peer = self.expected


def poll_until(session, condition, timeout=12):
    deadline = time.monotonic() + timeout
    while not condition() and time.monotonic() < deadline:
        session.poll(.025)
    assert condition(), 'timed out in v2 phase ' + session.phase


def receiver(session, sink):
    session.announce_screens([SCREEN])
    poll_until(session, lambda: sink.entered and sink.releases >= 2 and session.phase == 'local')
    assert sink.entered == SCREEN[0] and not session.keys and session.buttons == 0
    keys = [(f['usage'], f['down']) for kind, f in sink.events if kind == 'key']
    assert keys == COPY_KEYS, keys
    assert any(kind == 'move' for kind, _ in sink.events)
    assert any(kind == 'wheel' and f['vertical'] == 120 for kind, f in sink.events)


def host(session, sink):
    poll_until(session, lambda: bool(session.screens))
    screen_id = session.screens[0][0]
    session.enter(screen_id, 32768, 32768)
    poll_until(session, lambda: session.phase == 'remote')
    session.send_input('move', screen_id=screen_id, x=40000, y=32000)
    translator = ShortcutTranslator(os.environ['HOP_TEST_SHORTCUT_LIBRARY'], 3, session.peer_platform)
    try:
        for usage, down in COPY_KEYS:
            modifiers = 0 if (usage, down) == (0xe0,0) else 4
            for key in translator.push(usage=usage, modifiers=modifiers, down=down, repeat=0):
                session.send_input('key', **key)
        session.send_input('wheel', horizontal=0, vertical=120)
    finally:
        translator.close()
    time.sleep(.2)
    session.release()
    poll_until(session, lambda: session.phase == 'local')
    assert sink.restored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--isolated-interop', action='store_true', required=True)
    parser.add_argument('--role', choices=('host','receiver'), required=True)
    parser.add_argument('--tcp', choices=('dial','listen'), required=True)
    parser.add_argument('--address', default='127.0.0.1', type=ipaddress.IPv4Address)
    parser.add_argument('--port', default=0, type=int)
    parser.add_argument('--certificate', required=True, type=Path)
    parser.add_argument('--key', required=True, type=Path)
    parser.add_argument('--peer-certificate', required=True, type=Path)
    args = parser.parse_args()
    if not args.address.is_loopback:
        parser.error('loopback is required')
    lib = TLSLibrary(os.environ['HOP_TEST_TLS_LIBRARY'])
    cert, key = args.certificate.read_bytes(), args.key.read_bytes()
    peer_cert = args.peer_certificate.read_bytes()
    expected = VerifiedPeer(peer_cert, lib.certificate_fingerprint(peer_cert).lower())
    trust = SyntheticTrust(expected)
    initiator = args.tcp == 'dial'
    listener = None
    if initiator:
        connect = lambda: socket.create_connection((str(args.address), args.port), timeout=10)
    else:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.settimeout(25)
        listener.bind((str(args.address), args.port))
        listener.listen(4)
        print('LISTEN', listener.getsockname()[1], flush=True)
        connect = lambda: listener.accept()[0]
    try:
        candidate = exchange_candidate(connect(), own_role=1 if args.role=='host' else 2,
            name='Linux interop fixture', certificate=cert, tls_library=lib,
            explicitly_started=True, tcp_initiator=initiator)
        pending = PendingPairing(cert, candidate.certificate, lib.certificate_fingerprint)
        assert pending.peer_fingerprint.lower() == expected.fingerprint
        assert candidate.certificate == peer_cert
        pending.confirm(pending.local_fingerprint, pending.peer_fingerprint)
        sink = SyntheticSink()
        session = pair_verified_socket(connect(), pending=pending, library=lib, own_certificate=cert,
            private_key=key, role=args.role, tcp_initiator=initiator, sink=sink,
            trust_store=trust, capabilities=CAPABILITIES)
        try:
            assert session.transport.authenticated and session.transport.peer_fingerprint == expected.fingerprint
            assert session.peer_platform == 2 and session.peer_capabilities & 1 and trust.peer == expected
            print('PASS: HPOP half-close, HPSL, pinned mutual TLS and both pair confirmations', flush=True)
            (host if args.role == 'host' else receiver)(session, sink)
            print('PASS: Linux/Windows v2 focus, MOVE, Ctrl+C, wheel and RELEASE', flush=True)
            if args.role == 'receiver':
                time.sleep(.25)
        finally:
            session.close()
    finally:
        if listener:
            listener.close()


if __name__ == '__main__':
    main()
