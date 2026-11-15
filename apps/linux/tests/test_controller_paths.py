"""Controller fast paths: edge routing, emergency release, listener filtering."""
import queue
import socket
import threading
import time
import unittest
from types import SimpleNamespace
from unittest import mock

from hop_linux import controller as controller_module
from hop_linux.discovery_process import DiscoveryProcess
from hop_linux.layout import Device, Layout, Screen
from hop_linux.pair_offer import encode_selector, peek_selector
from hop_linux.x11_host_process import X11HostProcess

LOCAL, PEER, OTHER = 'a' * 64, 'b' * 64, 'c' * 64


def bare_host():
    host = object.__new__(controller_module.PairingController)
    host.local_id = LOCAL
    host.local_screens = ((1, 0, 0, 1000, 800),)
    host.layout = Layout((
        Device(LOCAL, 'Linux', 'linux', 0, 0, (Screen('1', 0, 0, 1000, 800),)),
        Device(PEER, 'Peer', 'macos', 1000, 0, (Screen('7', 0, 0, 900, 700),)),
    ))
    host.edge_observer = None
    host.probes = []
    host.started = []

    def snapshot(**options):
        host.probes.append(options)
        return host.local_screens, (999, 400)
    host._monitor_snapshot = snapshot
    host._start_control = lambda session, route, monitors=None: host.started.append((route, monitors))
    return host


def peer_session(peer=PEER):
    return SimpleNamespace(transport=SimpleNamespace(peer_fingerprint=peer), phase='local')


class EdgeEntryTests(unittest.TestCase):
    def test_edge_that_leads_nowhere_does_not_probe(self):
        host = bare_host()
        host._enter_from_local_edge(peer_session(), {'screen_id': 1, 'direction': 'left', 'x': 0, 'y': 30000})
        self.assertEqual(host.probes, [])
        self.assertEqual(host.started, [])

    def test_edge_toward_another_peer_does_not_probe(self):
        host = bare_host()
        host._enter_from_local_edge(peer_session(OTHER), {'screen_id': 1, 'direction': 'right', 'x': 65535, 'y': 30000})
        self.assertEqual(host.probes, [])

    def test_entry_probes_once_and_hands_the_result_on(self):
        host = bare_host()
        host._enter_from_local_edge(peer_session(), {'screen_id': 1, 'direction': 'right', 'x': 65535, 'y': 30000})
        self.assertEqual(host.probes, [{'require_input_clear': True, 'include_pointer': True}])
        (route, monitors), = host.started
        self.assertEqual(route.device_id, PEER)
        self.assertEqual(monitors, host.local_screens)

    def test_held_input_cancels_the_entry(self):
        host = bare_host()

        def busy(**_):
            raise controller_module.ControllerError('held')
        host._monitor_snapshot = busy
        host._enter_from_local_edge(peer_session(), {'screen_id': 1, 'direction': 'right', 'x': 65535, 'y': 30000})
        self.assertEqual(host.started, [])


class FakeSession:
    role = 'host'

    def __init__(self, sink):
        self.sink = sink; self.phase = 'remote'; self.released = 0; self.closed = False
        self.transport = SimpleNamespace(authenticated=True)

    def release(self):
        self.released += 1; self.phase = 'releasing'; self.sink.restore_local()

    def close(self):
        self.closed = True


class EmergencyTests(unittest.TestCase):
    def capture(self, on_emergency):
        capture = X11HostProcess(screen_id=7, target_size=(900, 700), capture_center=(500, 400), on_emergency=on_emergency)
        session = FakeSession(capture)
        capture.session = session; capture.emergency = True
        return capture, session

    def test_emergency_releases_focus_and_keeps_the_session(self):
        host = object.__new__(controller_module.PairingController)
        host.active_capture = None; host.returning_local = False
        capture, session = self.capture(lambda current: host._emergency_return(current))
        host.active_capture = capture
        capture.pump(session)
        self.assertEqual(session.released, 1)
        self.assertFalse(session.closed)
        self.assertIsNone(host.active_capture)
        self.assertTrue(host.returning_local)

    def test_without_a_handler_the_emergency_ends_the_session(self):
        capture, session = self.capture(None)
        with self.assertRaises(RuntimeError):
            capture.pump(session)
        self.assertTrue(session.closed)


class ListenerFilterTests(unittest.TestCase):
    def setUp(self):
        self.listener = socket.socket(); self.listener.bind(('127.0.0.1', 0)); self.listener.listen(4)
        self.addCleanup(self.listener.close)
        self.port = self.listener.getsockname()[1]
        self.clients = []

    def connect(self, data):
        client = socket.create_connection(('127.0.0.1', self.port))
        self.clients.append(client); self.addCleanup(client.close)
        if data:
            client.sendall(data)
        return client

    def host(self):
        host = object.__new__(controller_module.PairingController)
        host.listener = self.listener; host.stopping = threading.Event()
        host.socket_lock = threading.Lock(); host.active_socket = None
        return host

    def test_stray_connections_do_not_end_the_window(self):
        pending = SimpleNamespace(peer_fingerprint=PEER.upper())
        self.connect(b'GET / HTTP/1.1\r\n\r\n' + b'x' * 60)      # a scan
        self.connect(encode_selector(OTHER.upper()))                     # another trusted device
        self.connect(encode_selector(PEER.upper()) + b'tls')             # the confirmed peer
        sock = self.host()._accept_selected_peer(pending, 5)
        try:
            # The selector is still unread for the TLS handshake.
            self.assertEqual(sock.recv(71, socket.MSG_WAITALL), encode_selector(PEER.upper()) + b'tls')
        finally:
            sock.close()

    def test_window_expires_without_the_peer(self):
        pending = SimpleNamespace(peer_fingerprint=PEER)
        start = time.monotonic()
        with self.assertRaises(controller_module.ControllerError):
            self.host()._accept_selected_peer(pending, .3)
        self.assertLess(time.monotonic() - start, 3)

    def test_peek_waits_for_a_split_selector(self):
        client = self.connect(b'')
        server, _ = self.listener.accept()
        self.addCleanup(server.close)
        data = encode_selector(PEER.upper())
        client.sendall(data[:10])
        threading.Timer(.1, lambda: client.sendall(data[10:])).start()
        self.assertEqual(peek_selector(server, timeout=2), PEER.upper())


class DiscoveryQueueTests(unittest.TestCase):
    def test_full_queue_keeps_the_newest_items(self):
        discovery = DiscoveryProcess()
        for index in range(40):
            discovery._put({'type': 'hints', 'items': [index]})
        items = []
        while True:
            item = discovery.poll()
            if item is None:
                break
            items.append(item['items'][0])
        self.assertEqual(items[-1], 39)
        self.assertLessEqual(len(items), 16)


class LimitTests(unittest.TestCase):
    def test_wire_sized_names_and_screens_fit_a_layout(self):
        screens = tuple(Screen(str(i), i * 65535, 0, 65535, 65535) for i in range(1)) + \
            tuple(Screen(str(i), (i % 8) * 1000, (i // 8) * 1000, 1000, 1000) for i in range(1, 64))
        Device(PEER, 'n' * 128, 'windows', 0, 0, screens)
        with self.assertRaises(ValueError):
            Device(PEER, 'n' * 129, 'windows', 0, 0, screens[:1])


if __name__ == '__main__':
    unittest.main()
