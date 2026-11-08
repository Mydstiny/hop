#!/usr/bin/env python3
"""Offline conformance tests; do not claim these as physical-device injection tests."""
import json
from pathlib import Path
import random
import socket
import struct
import subprocess
import sys
import threading
import unittest

from hop import (Binding, Coalescer, Decoder, Frame, Hello, PairClient, PairServer,
                 Phase, ProtocolError, RemoteState, Type, Watchdog, greeting, message)

ROOT = Path(__file__).resolve().parents[2]
MAC_ID = bytes.fromhex('11111111111141118111111111111111')
PC_ID = bytes.fromhex('22222222222242228222222222222222')
TOKEN = bytes(range(1, 33))
NONCE = bytes(range(16))


def samples():
    return [
        ('HELLO', greeting(Hello(Phase.PAIR, 2, 1, 0, 1920, 1080, PC_ID, NONCE, name='Harmony PC'))),
        ('HELLO_ACK', greeting(Hello(Phase.CHALLENGE, 1, 1, 123456, 2560, 1600, MAC_ID, NONCE, name='Mac'))),
        ('ENTER', message(Type.ENTER, 70, 32768, 1, 1, epoch=1)),
        ('LEAVE', message(Type.LEAVE, 65500, 32768, 0, epoch=1)),
        ('REQUEST_LEAVE', message(Type.REQUEST_LEAVE, 0, 32768, epoch=1)),
        ('MOUSE_MOVE', message(Type.MOUSE_MOVE, 65535, 12345, epoch=1)),
        ('MOUSE_BUTTON', message(Type.MOUSE_BUTTON, 1, 1, 32768, 32768, 1, epoch=1)),
        ('WHEEL', message(Type.WHEEL, -120, 240, epoch=1)),
        ('KEY_DOWN', message(Type.KEY, 0, 64, 1, 0, epoch=1)),
        ('KEY_UP', message(Type.KEY, 0, 64, 0, 0, epoch=1)),
        ('KEEPALIVE', message(Type.KEEPALIVE, 123456789)),
        ('BYE', message(Type.BYE, 0)),
    ]


class CodecTests(unittest.TestCase):
    def test_all_ten_types_twelve_names(self):
        self.assertEqual(len(Type), 10)
        self.assertEqual(len(samples()), 12)
        for name, f in samples():
            with self.subTest(name=name):
                raw = f.encode()
                self.assertEqual(len(raw), 132)
                self.assertEqual(raw[:4], b'\x00\x00\x00\x80')
                decoded = Decoder().feed(raw)
                self.assertEqual(len(decoded), 1)
                self.assertEqual(decoded[0].encode(), raw)
                self.assertEqual(decoded[0].fields(), f.fields())

    def test_golden_file_is_current(self):
        expected = [{'name': name, 'type': int(f.type), 'epoch': f.epoch,
                     'payload_hex': f.body()[8:].hex(), 'hex': f.encode().hex()} for name, f in samples()]
        actual = json.loads((ROOT / 'protocol/golden-vectors.json').read_text())
        self.assertEqual(actual, expected)

    def test_manual_golden_header_and_offsets(self):
        move = dict(samples())['MOUSE_MOVE'].encode()
        self.assertEqual(move[:16].hex(), '000000800105000000000001ffff3039')
        key = dict(samples())['KEY_DOWN'].encode()
        self.assertEqual(key[12:18].hex(), '000000400100')
        self.assertEqual(dict(samples())['HELLO_ACK'].encode()[12:20].hex(), '020101000001e240')

    def test_bytewise_fragmentation(self):
        decoder, result = Decoder(), []
        data = b''.join(frame.encode() for _, frame in samples())
        for byte in data:
            result += decoder.feed(bytes([byte]))
        self.assertEqual([f.encode() for f in result], [f.encode() for _, f in samples()])
        decoder.eof()

    def test_random_fragmentation_and_coalescing(self):
        rng = random.Random(42)
        data = b''.join(f.encode() for _, f in samples()) * 10
        for _ in range(20):
            decoder, result, index = Decoder(), [], 0
            while index < len(data):
                size = rng.randrange(1, 512)
                result += decoder.feed(data[index:index + size])
                index += size
            self.assertEqual(b''.join(f.encode() for f in result), data)
            decoder.eof()

    def test_reject_length_before_allocation(self):
        for length in (0, 1, 127, 129, 0xffffffff):
            d = Decoder()
            with self.assertRaises(ProtocolError):
                d.feed(struct.pack('>I', length))
            self.assertEqual(len(d.buffer), 0)

    def test_eof_partial(self):
        for prefix in (b'\0', b'\0\0\0\x80', dict(samples())['MOUSE_MOVE'].encode()[:-1]):
            d = Decoder()
            d.feed(prefix)
            with self.assertRaises(ProtocolError):
                d.eof()

    def test_bad_version_type_flags_reserved(self):
        good = bytearray(dict(samples())['MOUSE_MOVE'].encode())
        for offset, value in ((4, 0), (4, 2), (5, 0), (5, 11), (6, 1), (7, 1), (131, 1)):
            b = good.copy()
            b[offset] = value
            with self.subTest(offset=offset, value=value), self.assertRaises(ProtocolError):
                Decoder().feed(b)

    def test_epoch_restrictions(self):
        for f in (message(Type.MOUSE_MOVE, 0, 0), message(Type.BYE, 0, epoch=1), message(Type.KEEPALIVE, 0, epoch=1)):
            with self.assertRaises(ProtocolError):
                f.encode()

    def test_button_bitmask_matches_action(self):
        for button, down, mask in ((0, 1, 1), (6, 1, 32), (1, 1, 0), (2, 0, 2), (1, 2, 1), (1, 1, 33)):
            with self.assertRaises(ProtocolError):
                message(Type.MOUSE_BUTTON, button, down, 0, 0, mask, epoch=1).encode()

    def test_key_constraints(self):
        for vals in ((128, 0, 1, 0), (0, 512, 1, 0), (0, 0, 2, 0), (0, 0, 1, 2), (0, 0, 0, 1)):
            with self.assertRaises(ProtocolError):
                message(Type.KEY, *vals, epoch=1).encode()

    def test_other_field_constraints(self):
        for f in (message(Type.WHEEL, 120001, 0, epoch=1), message(Type.WHEEL, 0, -120001, epoch=1),
                  message(Type.BYE, 5), message(Type.LEAVE, 0, 0, 3, epoch=1),
                  message(Type.ENTER, 0, 0, 512, 0, epoch=1), message(Type.ENTER, 0, 0, 0, 32, epoch=1)):
            with self.assertRaises(ProtocolError):
                f.encode()

    def test_hello_rejects_wrong_role_ack_utf8_padding(self):
        good = bytearray(dict(samples())['HELLO'].encode())
        for offset, value in ((6, 1), (13, 3), (14, 2), (15, 1), (131, 1), (123, 255)):
            b = good.copy()
            b[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ProtocolError):
                Decoder().feed(b)
        b = good.copy()
        b[92] = 255
        with self.assertRaises(ProtocolError):
            Decoder().feed(b)

    def test_hello_dimensions_identity_nonce_token(self):
        defaults = dict(phase=Phase.PAIR, role=2, direction=1, code=0, width=1920, height=1080, identity=PC_ID, nonce=NONCE)
        for update in ({'width': 0}, {'height': 65536}, {'identity': bytes(16)}, {'nonce': bytes(16)},
                       {'token': TOKEN}, {'code': 99999}, {'phase': Phase.CONFIRM},
                       {'phase': Phase.RESUME}, {'name': 'é' * 16}, {'name': 'A\0B'}):
            args = defaults | update
            with self.subTest(update=update), self.assertRaises(ProtocolError):
                greeting(Hello(**args)).encode()


class PairingTests(unittest.TestCase):
    def setUp(self):
        self.now = 100.0
        self.clock = lambda: self.now
        self.s = PairServer(MAC_ID, clock=self.clock)
        self.c = PairClient(PC_ID, clock=self.clock)

    def challenge(self):
        ack = self.s.receive(self.c.start())
        self.c.receive(ack)
        return ack

    def pair(self, client_first=True):
        self.challenge()
        if client_first:
            self.assertIsNone(self.s.receive(self.c.confirm()))
            commit = self.s.confirm()
        else:
            self.assertIsNone(self.s.confirm())
            commit = self.s.receive(self.c.confirm())
        self.assertIsNone(self.s.binding)
        self.assertFalse(self.s.ready)
        client_commit = self.c.receive(commit)
        ready = self.s.receive(client_commit)
        self.c.receive(ready)

    def test_mutual_confirmation_client_first(self):
        self.pair()
        self.assertTrue(self.c.ready and self.s.ready)
        self.assertEqual(self.s.binding.token, self.c.binding.token)
        self.assertEqual(self.s.binding.identity, PC_ID)
        self.assertEqual(self.c.binding.identity, MAC_ID)

    def test_mutual_confirmation_server_first(self):
        self.pair(False)
        self.assertTrue(self.s.ready and self.c.ready)

    def test_left_direction_echoes_mac_challenge(self):
        self.s.direction = 0
        self.pair()
        self.assertEqual(self.c.direction, 0)
        self.assertTrue(self.s.ready and self.c.ready)

    def test_no_token_before_two_confirmations(self):
        challenge = self.challenge()
        self.assertEqual(challenge.fields().token, bytes(32))
        self.assertEqual(challenge.fields().code, self.c.code)
        self.assertIsNone(self.s.binding)
        self.assertIsNone(self.s.confirm())
        self.assertIsNone(self.s.binding)
        self.assertFalse(self.s.ready)

    def test_repeated_confirmation_no_second_commit(self):
        self.challenge()
        confirm = self.c.confirm()
        self.s.receive(confirm)
        self.assertIsNone(self.s.receive(confirm))
        self.assertIsNotNone(self.s.confirm())
        self.assertIsNone(self.s.confirm())

    def test_client_rejects_unsolicited_commit(self):
        self.challenge()
        with self.assertRaises(ProtocolError):
            self.c.receive(self.s.reply(Phase.COMMIT))
        self.assertIsNone(self.c.binding)

    def test_nonce_code_identity_mismatch(self):
        self.challenge()
        for patch in ({'nonce': b'\xff' * 16}, {'code': 999999 if self.c.code != 999999 else 888888}, {'identity': MAC_ID}):
            h = self.c.send(Phase.CONFIRM).fields()
            vals = h.__dict__ | patch
            with self.subTest(patch=patch), self.assertRaises(ProtocolError):
                self.s.receive(greeting(Hello(**vals)))
        self.assertIsNone(self.s.binding)

    def test_resume_correct_token_and_identity(self):
        self.pair()
        self.s.reset()
        c = PairClient(PC_ID, self.c.binding)
        self.s.started = self.s.clock()
        ack = self.s.receive(c.start())
        c.receive(ack)
        self.assertTrue(c.ready and self.s.ready)

    def test_wrong_token_or_uuid_no_pair_downgrade(self):
        for identity, token in ((PC_ID, b'x' * 32), (MAC_ID, TOKEN)):
            s = PairServer(MAC_ID, binding=Binding(PC_ID, TOKEN))
            c = PairClient(identity, Binding(MAC_ID, token))
            ack = s.receive(c.start())
            self.assertEqual(ack.fields().phase, Phase.REJECT)
            with self.assertRaises(ProtocolError):
                c.receive(ack)
            self.assertFalse(s.ready)

    def test_already_paired_rejects_stranger(self):
        s = PairServer(MAC_ID, binding=Binding(PC_ID, TOKEN))
        ack = s.receive(self.c.start())
        self.assertEqual(ack.fields().phase, Phase.REJECT)
        self.assertEqual(s.binding.token, TOKEN)

    def test_resume_validates_mac_uuid(self):
        c = PairClient(PC_ID, Binding(MAC_ID, TOKEN))
        h = Hello(Phase.READY, 1, 1, 0, 1920, 1080, PC_ID, c.nonce, TOKEN)
        with self.assertRaises(ProtocolError):
            c.receive(greeting(h))

    def test_pairing_expiry(self):
        self.challenge()
        self.now += 60
        with self.assertRaises(ProtocolError):
            self.s.confirm()
        with self.assertRaises(ProtocolError):
            self.c.receive(self.s.reply(Phase.CHALLENGE))

    def test_commit_ack_lost_can_resume_pending(self):
        self.challenge()
        self.s.confirm()
        commit = self.s.receive(self.c.confirm())
        self.c.receive(commit)  # don't deliver client ACK
        self.assertIsNone(self.s.binding)
        self.s.reset()
        c = PairClient(PC_ID, self.c.binding, clock=self.clock)
        c.receive(self.s.receive(c.start()))
        self.assertTrue(c.ready and self.s.ready)
        self.assertIsNotNone(self.s.binding)

    def test_commit_candidate_expires(self):
        self.challenge()
        self.s.confirm()
        self.c.receive(self.s.receive(self.c.confirm()))
        self.now += 61
        self.s.reset()
        c = PairClient(PC_ID, self.c.binding, clock=self.clock)
        ack = self.s.receive(c.start())
        self.assertEqual(ack.fields().phase, Phase.REJECT)
        self.assertIsNone(self.s.binding)


class StateTests(unittest.TestCase):
    def setUp(self):
        self.r = RemoteState()
        self.r.authenticated = self.r.authorized = True
        self.enter = message(Type.ENTER, 70, 32768, 0, 0, epoch=1)

    def test_no_input_without_authentication_or_permission(self):
        for authenticated, authorized in ((False, False), (True, False), (False, True)):
            self.r.authenticated, self.r.authorized = authenticated, authorized
            with self.assertRaises(ProtocolError):
                self.r.receive(self.enter)
            self.assertEqual(self.r.mode, 'local')

    def test_enter_ack_and_initial_pressed_state(self):
        f = message(Type.ENTER, 70, 32768, 65, 5, epoch=1)
        ack = self.r.receive(f)
        self.assertEqual(ack.encode(), f.encode())
        self.assertEqual(self.r.keys, {56, 55})
        self.assertEqual(self.r.buttons, {1, 3})

    def test_drag_click_wheel_and_leave_release(self):
        self.r.receive(self.enter)
        for f in (message(Type.MOUSE_BUTTON, 1, 1, 10000, 10000, 1, epoch=1),
                  message(Type.MOUSE_MOVE, 30000, 40000, epoch=1),
                  message(Type.WHEEL, -120, 240, epoch=1),
                  message(Type.KEY, 55, 64, 1, 0, epoch=1)):
            self.r.receive(f)
        self.assertEqual(self.r.buttons, {1})
        self.assertEqual(self.r.position, (30000, 40000))
        self.r.receive(message(Type.LEAVE, 0, 32768, 0, epoch=1))
        self.assertEqual((self.r.keys, self.r.buttons, self.r.mode), (set(), set(), 'local'))
        self.assertIn(('release_key', 55), self.r.events)
        self.assertIn(('release_button', 1), self.r.events)

    def test_stale_frames_rejected_and_new_epoch_accepted(self):
        self.r.receive(self.enter)
        with self.assertRaises(ProtocolError):
            self.r.receive(message(Type.MOUSE_MOVE, 0, 0, epoch=2))
        self.r.receive(message(Type.LEAVE, 0, 0, 0, epoch=1))
        with self.assertRaises(ProtocolError):
            self.r.receive(self.enter)
        self.r.receive(message(Type.ENTER, 70, 0, 0, 0, epoch=2))
        self.assertEqual(self.r.epoch, 2)

    def test_enter_epoch_wrap(self):
        self.r.last_epoch = 0xffffffff
        self.r.receive(self.enter)
        self.assertEqual(self.r.epoch, 1)

    def test_return_edge_requires_arming_both_directions(self):
        for direction in (0, 1):
            r = RemoteState(direction)
            r.authenticated = r.authorized = True
            edge = 0 if direction else 65535
            r.receive(message(Type.ENTER, edge, 32768, 0, 0, epoch=1))
            self.assertIsNone(r.receive(message(Type.MOUSE_MOVE, edge, 32768, epoch=1)))
            r.receive(message(Type.MOUSE_MOVE, 32768, 32768, epoch=1))
            r.receive(message(Type.KEY, 0, 0, 1, 0, epoch=1))
            request = r.receive(message(Type.MOUSE_MOVE, edge, 32768, epoch=1))
            self.assertEqual(request.type, Type.REQUEST_LEAVE)
            self.assertEqual(r.keys, set())
            self.assertEqual(r.mode, 'leaving')
            with self.assertRaises(ProtocolError):
                r.receive(message(Type.KEY, 0, 0, 1, 0, epoch=1))
            r.receive(message(Type.LEAVE, edge, 32768, 0, epoch=1))
            self.assertEqual(r.mode, 'local')

    def test_bye_disconnect_releases_idempotently(self):
        self.r.receive(self.enter)
        self.r.receive(message(Type.KEY, 56, 1, 1, 0, epoch=1))
        self.r.receive(message(Type.BYE, 2))
        self.r.release()
        self.assertFalse(self.r.keys)
        self.assertEqual(self.r.epoch, 0)

    def test_coalescing_only_latest_then_ordered_button(self):
        c = Coalescer()
        for x in range(1000):
            c.move(message(Type.MOUSE_MOVE, x, 1, epoch=1))
        button = message(Type.MOUSE_BUTTON, 1, 1, 999, 1, 1, epoch=1)
        result = c.flush(button)
        self.assertEqual(len(result), 2)
        self.assertEqual(result[0].fields(), (999, 1))
        self.assertEqual(result[1].type, Type.MOUSE_BUTTON)
        self.assertEqual(c.flush(), [])
        c.move(message(Type.MOUSE_MOVE, 0, 0, epoch=1))
        c.clear()
        self.assertEqual(c.flush(), [])

    def test_watchdog_monotonic_1s_and_3s(self):
        now = [100.0]
        w = Watchdog(lambda: now[0])
        now[0] = 100.999
        self.assertFalse(w.heartbeat_due())
        now[0] = 101
        self.assertTrue(w.heartbeat_due())
        w.sent_heartbeat()
        self.assertFalse(w.expired())
        now[0] = 102.999
        self.assertFalse(w.expired())
        now[0] = 103
        self.assertTrue(w.expired())
        w.received()
        self.assertFalse(w.expired())


class IntegrationTests(unittest.TestCase):
    def test_cli_pairing_and_remote_mouse_return_both_directions(self):
        for direction in (0, 1):
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]
            base = [sys.executable, str(ROOT / 'tools/mock-peer/hop.py')]
            common = ['--port', str(port), '--direction', str(direction), '--auto-confirm']
            server = subprocess.Popen(base + ['server'] + common + ['--exercise', '--duration', '1'],
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                first = server.stdout.readline()
                self.assertIn('listening', first)
                client = subprocess.run(base + ['client'] + common + ['--duration', '.2'],
                                        capture_output=True, text=True, timeout=8)
                server_output, _ = server.communicate(timeout=8)
                self.assertEqual(client.returncode, 0, client.stdout + client.stderr)
                self.assertEqual(server.returncode, 0, server_output)
                self.assertIn('AUTHENTICATED', client.stdout)
                self.assertIn('RETURNED to mock Mac', server_output)
            finally:
                if server.poll() is None:
                    server.kill()
                    server.communicate()

    def test_real_tcp_fragmented_binary_roundtrip(self):
        frames = [f for _, f in samples()]
        payload = b''.join(f.encode() for f in frames)
        listener = socket.socket()
        self.addCleanup(listener.close)
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        errors = []

        def server():
            try:
                conn, _ = listener.accept()
                with conn:
                    conn.settimeout(3)
                    decoder, count = Decoder(), 0
                    while count < len(frames):
                        data = conn.recv(13)
                        if not data:
                            raise AssertionError('early EOF')
                        for frame in decoder.feed(data):
                            raw = frame.encode()
                            for start in range(0, len(raw), 7):
                                conn.sendall(raw[start:start + 7])
                            count += 1
            except Exception as exc:
                errors.append(exc)

        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        with socket.create_connection(listener.getsockname(), timeout=3) as client:
            for start in range(0, len(payload), 11):
                client.sendall(payload[start:start + 11])
            output = b''
            while len(output) < len(payload):
                chunk = client.recv(37)
                self.assertTrue(chunk)
                output += chunk
        thread.join(timeout=3)
        self.assertFalse(thread.is_alive())
        self.assertFalse(errors)
        self.assertEqual(output, payload)

    def test_canonical_keymap_and_sdk_derived_coverage(self):
        data = json.loads((ROOT / 'protocol/keymap.json').read_text())
        self.assertEqual(data['version'], 1)
        keys = data['keys']
        self.assertEqual(len(keys), 120)
        self.assertEqual(len({k['mac'] for k in keys}), len(keys))
        bycode = {k['mac']: k for k in keys}
        self.assertEqual(bycode[0]['harmony'], 2017)
        self.assertEqual(bycode[55]['harmony'], 2076)
        self.assertEqual(bycode[54]['harmony'], 2077)
        self.assertEqual(bycode[115]['harmony'], 2081)  # NOT Android/system HOME=1
        self.assertIsNone(bycode[63]['harmony'])  # Fn is not guessed
        self.assertEqual({k['modifier_bit'] for k in keys if 'modifier_bit' in k}, set(range(9)))


if __name__ == '__main__':
    unittest.main(verbosity=2)
