import struct
import unittest
import zlib
from hop_linux.clipboard import (CHUNK, MAX_TEXT, Focus, SelectionOwner,
                                 SelectionReceiver, TEXT, PNG, validate_payload)


def png_chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(payload, zlib.crc32(kind)) & 0xffffffff)


def png(width=1, height=1):
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            png_chunk(b"IDAT", zlib.compress(b"\0\0\0\0\0")) + png_chunk(b"IEND", b""))


class ClipboardTests(unittest.TestCase):
    def setUp(self):
        self.clock = 0
        self.focus = Focus("peer-a", 5)
        self.owner = SelectionOwner(self.focus, now=lambda: self.clock)
        self.receiver = SelectionReceiver(self.focus, now=lambda: self.clock)

    def test_only_current_enabled_peer_and_epoch_can_install_or_read(self):
        for peer, epoch in (("peer-b", 5), ("peer-a", 4)):
            with self.assertRaises(PermissionError): self.owner.install_remote(peer, epoch, TEXT, b"secret")
            with self.assertRaises(PermissionError): self.receiver.begin(peer, epoch, TEXT)
        self.owner.focus = Focus("peer-a", 5, False)
        with self.assertRaises(PermissionError): self.owner.install_remote("peer-a", 5, TEXT, b"secret")

    def test_text_and_png_payload_validation(self):
        validate_payload(TEXT, "你好".encode())
        for text in (b'/home/user/file.txt',b'C:\\Users\\file.txt',b'file:///tmp/file.txt'):
            validate_payload(TEXT,text)
        validate_payload(TEXT, b"x" * MAX_TEXT)
        validate_payload(PNG, png())
        for content in (b"\xff", b"x" * (MAX_TEXT + 1)):
            with self.assertRaises(ValueError): validate_payload(TEXT, content)
        for content in (png(4097, 4097), png(16385, 1), png()[:-1], png() + b"tail", png()[:-5] + b"xx"):
            with self.assertRaises(ValueError): validate_payload(PNG, content)
        with self.assertRaises(ValueError): validate_payload("text/uri-list", b"file:///secret")

    def test_direct_selection_and_incr_round_trip(self):
        token = self.receiver.begin("peer-a", 5, TEXT)
        self.assertEqual(self.receiver.initial(token, TEXT, 8, b"hello"), b"hello")
        token = self.receiver.begin("peer-a", 5, TEXT)
        self.assertIsNone(self.receiver.initial(token, "INCR", 32, 3))
        self.assertIsNone(self.receiver.chunk(token, TEXT, 8, b"hel"))
        self.assertIsNone(self.receiver.chunk(token, TEXT, 8, b"lo"))
        self.assertEqual(self.receiver.chunk(token, TEXT, 8, b""), b"hello")

    def test_incr_lower_bound_is_not_assumed_actual_size(self):
        token = self.receiver.begin("peer-a", 5, TEXT)
        self.receiver.initial(token, "INCR", 32, 1)
        self.receiver.chunk(token, TEXT, 8, b"larger")
        self.assertEqual(self.receiver.chunk(token, TEXT, 8, b""), b"larger")

    def test_rejects_wrong_type_oversized_and_duplicate_incr(self):
        token = self.receiver.begin("peer-a", 5, TEXT)
        with self.assertRaises(ValueError): self.receiver.initial(token, "INCR", 32, MAX_TEXT + 1)
        token = self.receiver.begin("peer-a", 5, TEXT)
        self.receiver.initial(token, "INCR", 32, 1)
        with self.assertRaises(ValueError): self.receiver.chunk(token, PNG, 8, b"x")
        token = self.receiver.begin("peer-a", 5, TEXT)
        self.receiver.initial(token, "INCR", 32, 1)
        with self.assertRaises(ValueError): self.receiver.initial(token, "INCR", 32, 1)
        token = self.receiver.begin("peer-a", 5, TEXT)
        self.receiver.initial(token, "INCR", 32, 1)
        with self.assertRaises(ValueError): self.receiver.chunk(token, TEXT, 8, b"x" * (MAX_TEXT + 1))

    def test_stale_callback_cannot_overwrite_new_transfer(self):
        old = self.receiver.begin("peer-a", 5, TEXT)
        new = self.receiver.begin("peer-a", 5, TEXT)
        with self.assertRaisesRegex(RuntimeError, "stale"): self.receiver.initial(old, TEXT, 8, b"old")
        self.assertEqual(self.receiver.initial(new, TEXT, 8, b"new"), b"new")

    def test_focus_change_and_timeout_cancel_partial_data(self):
        token = self.receiver.begin("peer-a", 5, TEXT)
        self.receiver.initial(token, "INCR", 32, 1)
        self.receiver.chunk(token, TEXT, 8, b"private")
        self.receiver.focus = Focus("peer-b", 6)
        with self.assertRaisesRegex(RuntimeError, "focus changed"): self.receiver.chunk(token, TEXT, 8, b"")
        self.receiver.focus = self.focus
        token = self.receiver.begin("peer-a", 5, TEXT)
        self.clock = 11
        with self.assertRaisesRegex(RuntimeError, "expired"): self.receiver.initial(token, TEXT, 8, b"x")

    def test_owner_targets_and_supported_mime_only(self):
        self.owner.install_remote("peer-a", 5, PNG, png())
        self.assertEqual(self.owner.request(7, "PROP", "TARGETS"), ("ATOM", ("TARGETS", PNG)))
        self.assertEqual(self.owner.request(7, "PROP", PNG), (PNG, png()))
        self.assertIsNone(self.owner.request(7, "PROP", TEXT))
        with self.assertRaises(ValueError): self.owner.request(0, "PROP", PNG)

    def test_outgoing_incr_snapshot_survives_clipboard_change(self):
        data = b"a" * (CHUNK + 5)
        self.owner.install_remote("peer-a", 5, TEXT, data)
        self.assertEqual(self.owner.request(7, "PROP", TEXT), ("INCR", len(data)))
        self.owner.install_remote("peer-a", 5, TEXT, b"new")
        self.assertEqual(self.owner.property_deleted(7, "PROP"), (TEXT, data[:CHUNK]))
        self.owner.selection_lost()
        self.assertEqual(self.owner.property_deleted(7, "PROP"), (TEXT, data[CHUNK:]))
        self.assertEqual(self.owner.property_deleted(7, "PROP"), (TEXT, b""))
        self.assertIsNone(self.owner.property_deleted(7, "PROP"))

    def test_bounded_requestors_and_timeout(self):
        self.owner.install_remote("peer-a", 5, TEXT, b"a" * (CHUNK + 1))
        for i in range(1, 5): self.assertEqual(self.owner.request(i, "P", TEXT)[0], "INCR")
        self.assertIsNone(self.owner.request(5, "P", TEXT))
        self.clock = 11
        self.assertIsNone(self.owner.property_deleted(1, "P"))
        self.assertEqual(self.owner.request(5, "P", TEXT)[0], "INCR")
