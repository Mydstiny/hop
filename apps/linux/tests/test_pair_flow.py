import threading
import time
import unittest
from unittest import mock

from hop_linux import controller as controller_module
from hop_linux import pair_flow
from hop_linux.pairing import PendingPairing
from test_pairing import cert, fingerprint


def confirmed_pending():
    a, b = cert(b'a'), cert(b'b')
    pending = PendingPairing(a, b, fingerprint)
    pending.confirm(fingerprint(a), fingerprint(b))
    return pending


class Stop(Exception):
    pass


class PairFlowTimeoutTests(unittest.TestCase):
    def run_initiator(self, **options):
        seen = {}

        def connect(sock, **kwargs):
            seen['timeout'] = kwargs['timeout']
            raise Stop()

        with mock.patch.object(pair_flow, 'connect_selected', connect):
            with self.assertRaises(Stop):
                pair_flow.pair_verified_socket(mock.Mock(), pending=confirmed_pending(), library=None,
                    own_certificate=b'', private_key=b'', role='host', tcp_initiator=True,
                    sink=None, trust_store=None, **options)
        return seen['timeout']

    def test_initiator_waits_for_the_other_users_confirmation(self):
        # The listener accepts only after its own user confirms, so the
        # initiator's TLS handshake may legitimately wait most of the window.
        self.assertEqual(self.run_initiator(connect_timeout=95), 95)

    def test_connect_timeout_defaults_to_the_session_timeout(self):
        self.assertEqual(self.run_initiator(timeout=12), 12)

    def test_connect_timeout_is_bounded(self):
        for value in (0, -1, 121, 'x'):
            with self.assertRaises(ValueError):
                pair_flow.pair_verified_socket(mock.Mock(), pending=confirmed_pending(), library=None,
                    own_certificate=b'', private_key=b'', role='host', tcp_initiator=True,
                    sink=None, trust_store=None, connect_timeout=value)


class ControllerPairingTests(unittest.TestCase):
    def bare_receiver(self):
        receiver = object.__new__(controller_module.ReceiverPairingController)
        receiver.socket_lock = threading.Lock(); receiver.stopping = threading.Event()
        receiver.active_socket = None; receiver.enable_input = False
        receiver.library = receiver.certificate = receiver.private_key = receiver.store = None
        receiver._new_clipboard = lambda: None
        receiver._event = lambda item: None
        return receiver

    def test_pairing_only_receiver_advertises_the_mandatory_focus_capability(self):
        # Core hosts (Windows, macOS, HarmonyOS) end a session whose HELLO lacks
        # capability bit 0. No screens are announced, so no ENTER can target it.
        seen = {}

        def pair(sock, **kwargs):
            seen.update(kwargs)
            raise Stop()

        receiver = self.bare_receiver(); receiver.peer_confirm_deadline = time.monotonic() + 80
        with mock.patch.object(controller_module, 'pair_verified_socket', pair):
            with self.assertRaises(Stop):
                receiver._serve_receiver(mock.Mock(), tcp_initiator=True, peer_name='host', pending=confirmed_pending())
        self.assertEqual(seen['capabilities'] & 1, 1)
        self.assertGreater(seen['connect_timeout'], 70)
        self.assertLessEqual(seen['connect_timeout'], 120)

    def test_confirmation_budget_covers_the_rest_of_the_review_window(self):
        receiver = self.bare_receiver()
        receiver.peer_confirm_deadline = time.monotonic() + 100
        self.assertGreater(receiver._peer_confirm_budget(), 95)
        receiver.peer_confirm_deadline = time.monotonic() + 1
        self.assertEqual(receiver._peer_confirm_budget(), 15.0)
        receiver.peer_confirm_deadline = time.monotonic() + 500
        self.assertEqual(receiver._peer_confirm_budget(), 120.0)


if __name__ == '__main__':
    unittest.main()
