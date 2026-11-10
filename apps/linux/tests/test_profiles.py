import math
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from hop_linux.profiles import PointerProfile, ProfileStore


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="hop-profile-test-")
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "config" / "profiles.json"
        self.store = ProfileStore(self.path)
        self.a, self.b = "a" * 64, "b" * 64

    def test_per_peer_sensitivity_and_scroll_are_independent(self):
        first = PointerProfile(movement=2, horizontal_scroll=.5, vertical_scroll=3,
                               reverse_horizontal=True, reverse_vertical=False)
        second = PointerProfile(movement=.5, reverse_vertical=True, forward_hz=60)
        self.assertEqual(self.store.get(self.a), PointerProfile())
        self.store.update(self.a, first); self.store.update(self.b, second)
        self.assertEqual(self.store.get(self.a), first)
        self.assertEqual(self.store.get(self.b), second)
        self.assertEqual(first.move(4, -2), (8, -4))
        self.assertEqual(first.scroll(2, 3), (-1, 9))
        self.assertEqual(second.scroll(2, 3), (2, -3))
        self.assertEqual((first.forward_hz,second.forward_hz),(240,60))
        self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o600)

    def test_invalid_values_and_unverified_identifier_rejected(self):
        for value in (0, 4.01, math.inf, math.nan, True):
            with self.assertRaises(ValueError): PointerProfile(movement=value)
        with self.assertRaises(ValueError): PointerProfile(reverse_horizontal=1)
        for frequency in (19,1001,True,120.0,200):
            with self.assertRaises(ValueError):PointerProfile(forward_hz=frequency)
        for peer in ("host.local", "", "g" * 64):
            with self.assertRaises(ValueError): self.store.get(peer)
        with self.assertRaises(ValueError): PointerProfile().move(float("nan"), 1)
        with self.assertRaises(ValueError): PointerProfile().scroll(1000001, 0)

    def test_atomic_failure_leaves_existing_profile_intact(self):
        old = PointerProfile(movement=2)
        self.store.update(self.a, old)
        before = self.path.read_bytes()
        with patch("hop_linux.profiles.os.replace", side_effect=OSError("synthetic failure")):
            with self.assertRaises(OSError): self.store.update(self.a, PointerProfile(movement=3))
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(self.store.get(self.a), old)
        self.assertEqual(list(self.path.parent.glob(".hop-profile-*")), [])

    def test_corrupt_or_duplicate_json_preserves_original(self):
        self.path.parent.mkdir(parents=True)
        for raw in (b"not-json", b'{"version":1,"peers":{},"peers":{}}', b'{"version":2,"peers":{}}'):
            self.path.write_bytes(raw)
            with self.assertRaises(ValueError): self.store.update(self.a, PointerProfile())
            self.assertEqual(self.path.read_bytes(), raw)

    def test_symlink_is_not_followed(self):
        self.path.parent.mkdir(parents=True)
        target = Path(self.tmp.name) / "unrelated.json"
        target.write_bytes(b"do not modify")
        self.path.symlink_to(target)
        with self.assertRaises(ValueError): self.store.update(self.a, PointerProfile())
        self.assertEqual(target.read_bytes(), b"do not modify")
        self.assertTrue(self.path.is_symlink())

    def test_same_fingerprint_case_resolves_same_device(self):
        self.store.update(self.a.upper(), PointerProfile(movement=1.5))
        self.assertEqual(self.store.get(self.a).movement, 1.5)

    def test_concurrent_peer_updates_do_not_overwrite_each_other(self):
        failures = []
        peers = [format(i, "064x") for i in range(1, 9)]
        def update(peer):
            try: self.store.update(peer, PointerProfile(movement=2))
            except Exception as exc: failures.append(exc)
        threads = [threading.Thread(target=update, args=(peer,)) for peer in peers]
        for thread in threads: thread.start()
        for thread in threads: thread.join()
        self.assertFalse(failures)
        for peer in peers: self.assertEqual(self.store.get(peer).movement, 2)

    def test_legacy_profile_defaults_frequency_without_rewrite(self):
        import json
        self.path.parent.mkdir(parents=True)
        original=json.dumps({'version':1,'peers':{self.a:{'movement':1,'horizontal_scroll':1,'vertical_scroll':1,'reverse_horizontal':False,'reverse_vertical':False}}}).encode()
        self.path.write_bytes(original)
        self.assertEqual(self.store.get(self.a).forward_hz,240)
        self.assertEqual(self.path.read_bytes(),original)


if __name__ == "__main__": unittest.main()
