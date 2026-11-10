from pathlib import Path
import tempfile
import unittest
from hop_linux.editor import LayoutEditor
from hop_linux.layout import LayoutStore, sample_layout
from hop_linux.profiles import PointerProfile, ProfileStore


class EditorTests(unittest.TestCase):
    def test_device_snap_and_invalid_overlap_reverts(self):
        editor = LayoutEditor(sample_layout())
        editor.start_drag("b" * 64)
        self.assertFalse(editor.drag_to(910, 0).errors)
        self.assertEqual(editor.layout.devices[1].x, 900)  # snap to Mac
        self.assertTrue(editor.end_drag())
        editor.start_drag("b" * 64)
        self.assertTrue(editor.drag_to(20, 20, snap=False).errors)
        self.assertFalse(editor.end_drag())
        self.assertEqual(editor.layout.devices[1].x, 900)
    def test_expanded_screen_moves_independently(self):
        editor = LayoutEditor(sample_layout()); editor.select("b" * 64)
        with self.assertRaises(ValueError): editor.start_drag("b" * 64, "harmony-extra")
        editor.toggle(); editor.start_drag("b" * 64, "harmony-extra")
        editor.drag_to(800, 500, snap=False)
        self.assertTrue(editor.end_drag())
        self.assertEqual(editor.layout.devices[1].screens[1].y, 500)
        self.assertEqual(editor.layout.devices[1].screens[0].y, 0)
    def test_independent_explicit_persistence(self):
        with tempfile.TemporaryDirectory(prefix="hop-editor-test-") as directory:
            path = Path(directory)
            editor = LayoutEditor(sample_layout(), LayoutStore(path / "layout.json"), ProfileStore(path / "profiles.json"))
            self.assertFalse((path / "layout.json").exists())
            editor.set_profile("a" * 64, PointerProfile(movement=2))
            editor.set_profile("b" * 64, PointerProfile(movement=.5, reverse_vertical=True))
            self.assertEqual(editor.profile("a" * 64).movement, 2)
            self.assertEqual(editor.profile("b" * 64).movement, .5)
            editor.save()
            self.assertEqual(LayoutStore(path / "layout.json").load().to_data(), editor.layout.to_data())
