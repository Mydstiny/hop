import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from hop_linux.layout import Device, Layout, LayoutStore, Screen, sample_layout


def device(label, x, y, w=100, h=100):
    return Device(label * 64, label, "linux", x, y, (Screen(label, 0, 0, w, h),))


class LayoutTests(unittest.TestCase):
    def test_three_devices_four_screens_are_connected(self):
        layout = sample_layout()
        self.assertEqual(len(layout.devices), 3)
        self.assertEqual(sum(len(d.screens) for d in layout.devices), 4)
        self.assertFalse(layout.validate().errors)
        self.assertFalse(layout.validate().warnings)
        self.assertEqual(len(layout.validate().edges), 3)
    def test_edges_require_real_overlap_not_corner(self):
        a = device("a", 0, 0)
        self.assertFalse(Layout((a, device("b", 100, 50))).validate().warnings)
        self.assertEqual(Layout((a, device("b", 100, 100))).validate().warnings, ("disconnected_devices",))
        self.assertEqual(Layout((a, device("b", 101, 0))).validate().warnings, ("disconnected_devices",))
    def test_overlap_and_invalid_move(self):
        layout = Layout((device("a", 0, 0), device("b", 100, 0)))
        moved = layout.move("b" * 64, 99, 0)
        self.assertTrue(moved.validate().errors)
        self.assertFalse(layout.validate().errors)
        with self.assertRaises(ValueError): layout.move("missing", 0, 0)
        with self.assertRaises(ValueError): layout.move("b" * 64, True, 0)
    def test_device_screens_are_individually_checked(self):
        d = Device("a" * 64, "multi", "linux", 0, 0,
                   (Screen("main", 0, 0, 100, 100), Screen("other", 90, 0, 100, 100)))
        self.assertTrue(Layout((d,)).validate().errors)
        with self.assertRaises(ValueError):
            Device("a" * 64, "invalid", "linux", 0, 0, (Screen("same", 0, 0, 1, 1), Screen("same", 1, 0, 1, 1)))
    def test_move_individual_screen_does_not_move_others(self):
        layout = sample_layout()
        original = layout.devices[1]
        moved = layout.move_screen("b" * 64, "harmony-extra", 800, 500)
        self.assertEqual(moved.devices[1].screens[0], original.screens[0])
        self.assertEqual(moved.devices[1].screens[1].y, 500)
        self.assertEqual(layout.devices[1].screens[1].y, 0)
        with self.assertRaises(ValueError): layout.move_screen("b" * 64, "missing", 0, 0)
    def test_malformed_documents_and_duplicate_devices_rejected(self):
        layout = sample_layout(); data = layout.to_data()
        self.assertEqual(Layout.from_data(data).to_data(), data)
        data["devices"].append(data["devices"][0])
        with self.assertRaises(ValueError): Layout.from_data(data)
        with self.assertRaises(ValueError): Layout.from_data({"version": 1, "devices": [], "unknown": 1})


class LayoutStoreTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="hop-layout-test-")
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "config" / "layout.json"
        self.store = LayoutStore(self.path)
    def test_explicit_save_and_reload(self):
        self.assertFalse(self.path.exists())
        layout = sample_layout(); self.store.save(layout)
        self.assertEqual(self.store.load().to_data(), layout.to_data())
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)
    def test_overlap_cannot_replace_saved_layout(self):
        layout = sample_layout(); self.store.save(layout); original = self.path.read_bytes()
        with self.assertRaises(ValueError): self.store.save(layout.move("b" * 64, 10, 10))
        self.assertEqual(self.path.read_bytes(), original)
    def test_atomic_write_failure_preserves_previous_bytes(self):
        self.store.save(sample_layout()); original = self.path.read_bytes()
        with patch("hop_linux.layout.os.replace", side_effect=OSError("synthetic")):
            with self.assertRaises(OSError): self.store.save(sample_layout())
        self.assertEqual(self.path.read_bytes(), original)
        self.assertEqual(list(self.path.parent.glob(".hop-layout-*")), [])
    def test_symlink_and_corruption_not_overwritten(self):
        self.path.parent.mkdir(parents=True)
        target = Path(self.tmp.name) / "other"; target.write_bytes(b"keep")
        self.path.symlink_to(target)
        with self.assertRaises(ValueError): self.store.save(sample_layout())
        self.assertEqual(target.read_bytes(), b"keep")
        self.path.unlink(); self.path.write_bytes(b'{"version":1,"devices":[],"devices":[]}')
        with self.assertRaises(ValueError): self.store.load()
        self.assertEqual(self.path.read_bytes(), b'{"version":1,"devices":[],"devices":[]}')
