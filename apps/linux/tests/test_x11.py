"""Tests use only an in-memory fake; no X server or input events are touched."""
import threading
import unittest
from unittest.mock import patch

from hop_linux.x11 import Event, X11Backend
from hop_linux.keymap import KeyMap


class FakeNative:
    def __init__(self):
        self.calls = []
        self.keyboard_result = 0
        self.pointer_result = 0
        self.extension = True
        self.failed_keys = set()
        self.failed_buttons = set()
        self.items = []
    def open(self): self.calls.append("open"); return 100
    def xtest_supported(self, d): self.calls.append("query_xtest"); return self.extension
    def root(self, d): self.calls.append("root"); return 101
    def key_names(self, d): return {99: b"LCTL", 101: b"AC01"}
    def grab_keyboard(self, d, root): self.calls.append("grab_keyboard"); return self.keyboard_result
    def grab_pointer(self, d, root): self.calls.append("grab_pointer"); return self.pointer_result
    def ungrab_keyboard(self, d): self.calls.append("ungrab_keyboard")
    def ungrab_pointer(self, d): self.calls.append("ungrab_pointer")
    def flush(self, d): self.calls.append("flush")
    def close(self, d): self.calls.append("close")
    def events(self, d, limit): self.calls.append(("events", limit)); return self.items[:limit]
    def key(self, d, code, down):
        self.calls.append(("key", code, down))
        return (code, down) not in self.failed_keys
    def button(self, d, code, down):
        self.calls.append(("button", code, down))
        return (code, down) not in self.failed_buttons
    def motion(self, d, x, y): self.calls.append(("motion", x, y)); return True


class X11Tests(unittest.TestCase):
    def setUp(self):
        self.native = FakeNative()
        self.backend = X11Backend(self.native)
    def tearDown(self):
        if self.backend._display is not None:
            self.backend.close()

    def test_no_grab_or_injection_during_construction(self):
        self.assertEqual(self.native.calls, ["open", "query_xtest", "root"])
        self.assertEqual(self.backend.events(), [])
    def test_pointer_grab_failure_rolls_back_keyboard_before_return(self):
        self.native.pointer_result = 1
        with self.assertRaisesRegex(RuntimeError, "pointer grab denied"):
            self.backend.grab()
        self.assertEqual(self.native.calls[-3:], ["grab_pointer", "ungrab_keyboard", "flush"])
        self.assertFalse(self.backend._grabbed)
    def test_keyboard_grab_failure_never_grabs_pointer(self):
        self.native.keyboard_result = 1
        with self.assertRaisesRegex(RuntimeError, "keyboard grab denied"):
            self.backend.grab()
        self.assertNotIn("grab_pointer", self.native.calls)
    def test_grab_and_ungrab_are_idempotent(self):
        self.backend.grab(); self.backend.grab()
        self.backend.ungrab(); self.backend.ungrab()
        self.assertEqual(self.native.calls.count("grab_keyboard"), 1)
        self.assertEqual(self.native.calls.count("grab_pointer"), 1)
        self.assertEqual(self.native.calls.count("ungrab_pointer"), 1)
        self.assertEqual(self.native.calls.count("ungrab_keyboard"), 1)
        self.assertLess(self.native.calls.index("ungrab_pointer"), self.native.calls.index("ungrab_keyboard"))
    def test_events_only_while_grabbed_and_bounded(self):
        self.native.items = [Event("key", 42, True)]
        self.assertEqual(self.backend.events(), [])
        self.backend.grab()
        self.assertEqual(self.backend.events(1), self.native.items)
        for bad in (0, -1, 1025, 1.5):
            with self.assertRaises(ValueError): self.backend.events(bad)
    def test_actual_pressed_sets_release_once(self):
        self.backend.key(38, True); self.backend.button(1, True)
        self.assertEqual(self.backend.release_all(), [])
        self.assertEqual(self.backend.release_all(), [])
        self.assertEqual(self.native.calls.count(("key", 38, False)), 1)
        self.assertEqual(self.native.calls.count(("button", 1, False)), 1)
    def test_failed_release_is_retained_and_retryable(self):
        self.backend.key(38, True); self.backend.button(1, True)
        self.native.failed_keys.add((38, False))
        self.assertEqual(self.backend.release_all(), [("key", 38)])
        self.assertIn(("button", 1, False), self.native.calls)
        self.native.failed_keys.clear()
        self.assertEqual(self.backend.release_all(), [])
        self.assertEqual(self.native.calls.count(("key", 38, False)), 2)
    def test_failed_injection_is_not_recorded_as_success(self):
        self.native.failed_keys.add((38, True))
        with self.assertRaisesRegex(RuntimeError, "injection failed"):
            self.backend.key(38, True)
        self.assertEqual(self.backend.release_all(), [])
        self.assertNotIn(("key", 38, False), self.native.calls)
    def test_close_restores_grabs_and_releases_before_close(self):
        self.backend.grab(); self.backend.key(38, True); self.backend.button(1, True)
        self.backend.close()
        self.assertLess(self.native.calls.index("ungrab_pointer"), self.native.calls.index(("key", 38, False)))
        self.assertLess(self.native.calls.index(("button", 1, False)), self.native.calls.index("close"))
        self.backend.close()
    def test_close_reports_failed_release_but_still_closes(self):
        self.backend.key(38, True); self.backend.button(1, True)
        self.native.failed_keys.add((38, False))
        with self.assertRaisesRegex(RuntimeError, "cleanup failed"):
            self.backend.close()
        self.assertIn(("button", 1, False), self.native.calls)
        self.assertEqual(self.native.calls[-1], "close")
    def test_hid_injection_releases_original_code_after_keymap_changes(self):
        old = self.backend.keymap()
        self.backend.key_hid(old, 0xE0, True)
        changed = KeyMap.from_names({33: "LCTL", 35: "AC01"})
        self.backend.key_hid(changed, 0xE0, True)  # autorepeat retains old keycode
        self.backend.key_hid(changed, 0xE0, False)
        self.assertEqual(self.native.calls.count(("key", 99, True)), 2)
        self.assertIn(("key", 99, False), self.native.calls)
        self.assertNotIn(("key", 33, False), self.native.calls)
        self.assertFalse(self.backend.key_hid(changed, 0xE0, False))
        with self.assertRaisesRegex(ValueError, "missing or ambiguous"):
            self.backend.key_hid(changed, 0xE7, True)
    def test_hid_release_cleanup_retains_failed_key_for_retry(self):
        keymap = self.backend.keymap()
        self.backend.key_hid(keymap, 0xE0, True)
        self.native.failed_keys.add((99, False))
        self.assertEqual(self.backend.release_all(), [("key", 99)])
        self.native.failed_keys.clear()
        self.assertTrue(self.backend.key_hid(keymap, 0xE0, False))
        self.assertFalse(self.backend.key_hid(keymap, 0xE0, False))
    def test_values_outside_supported_range_rejected(self):
        for key in (-1, 0, 7, 256, True):
            with self.assertRaises(ValueError): self.backend.key(key, True)
        for button in (0, 4, 5, 6, 7, 10, True):
            with self.assertRaises(ValueError): self.backend.button(button, True)
        for x, y in ((-1, 0), (0, 32768), (True, 0)):
            with self.assertRaises(ValueError): self.backend.motion(x, y)
    def test_wheel_maps_both_axes_and_cleans_up_failed_release(self):
        self.backend.scroll(1, -1)
        self.assertIn(("button", 7, True), self.native.calls)
        self.assertIn(("button", 4, True), self.native.calls)
        self.native.failed_buttons.add((5, False))
        with self.assertRaisesRegex(RuntimeError, "wheel release failed"):
            self.backend.scroll(0, 1)
        self.native.failed_buttons.clear()
        self.assertEqual(self.backend.release_all(), [])
        self.assertEqual(self.native.calls.count(("button", 5, False)), 2)
        for bad in (101, -101, True, 0.5):
            with self.assertRaises(ValueError): self.backend.scroll(bad, 0)
    def test_backend_is_thread_owned(self):
        errors = []
        def call():
            try: self.backend.grab()
            except Exception as e: errors.append(str(e))
        thread = threading.Thread(target=call); thread.start(); thread.join()
        self.assertEqual(errors, ["X11 backend must be used on its owning thread"])
        self.assertNotIn("grab_keyboard", self.native.calls)
    def test_missing_xtest_closes_display(self):
        fake = FakeNative(); fake.extension = False
        with self.assertRaisesRegex(RuntimeError, "does not support XTest"):
            X11Backend(fake)
        self.assertEqual(fake.calls[-1], "close")
    def test_wayland_does_not_silently_use_xwayland(self):
        with patch.dict("os.environ", {"WAYLAND_DISPLAY": "wayland-0"}, clear=True):
            with self.assertRaisesRegex(RuntimeError, "XWayland"):
                X11Backend()


if __name__ == "__main__": unittest.main()
