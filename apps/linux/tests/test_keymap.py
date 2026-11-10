import unittest
from hop_linux.keymap import HeldKeyTranslator, KeyMap


class KeyMapTests(unittest.TestCase):
    def test_server_names_not_evdev_offsets_determine_mapping(self):
        # Deliberately shuffled numbers: mapping must follow physical XKB names.
        mapping = KeyMap.from_names({199: b"AC01", 21: b"AD01", 88: b"AB01", 49: b"AE01"})
        self.assertEqual(mapping.to_hid(199), 0x04)  # A
        self.assertEqual(mapping.to_hid(21), 0x14)   # Q
        self.assertEqual(mapping.to_hid(88), 0x1D)   # Z
        self.assertEqual(mapping.to_hid(49), 0x1E)   # 1
        self.assertEqual(mapping.to_x11(0x04), 199)
        self.assertIsNone(mapping.to_hid(38))  # no guessed evdev default
    def test_left_and_right_modifiers_remain_distinct(self):
        names = ["LCTL", "LFSH", "LALT", "LWIN", "RCTL", "RTSH", "RALT", "RWIN"]
        keymap = KeyMap.from_names({i + 40: name for i, name in enumerate(names)})
        for index in range(8):
            self.assertEqual(keymap.to_hid(index + 40), 0xE0 + index)
            self.assertEqual(keymap.to_x11(0xE0 + index), index + 40)
    def test_ambiguous_usage_never_maps_both_ways(self):
        keymap = KeyMap.from_names({10: "AC01", 11: "AC01", 12: "UNKNOWN", 13: b"\xff"})
        self.assertEqual(keymap.ambiguous, frozenset({0x04}))
        self.assertEqual(keymap.unsupported, frozenset({12, 13}))
        self.assertIsNone(keymap.to_hid(10)); self.assertIsNone(keymap.to_hid(11))
        self.assertIsNone(keymap.to_x11(0x04))
    def test_hold_repeat_and_release_survive_map_change(self):
        a = KeyMap.from_names({13: "LCTL", 14: "AC01"})
        b = KeyMap.from_names({23: "LCTL", 24: "AC01"})
        trans = HeldKeyTranslator(a)
        self.assertEqual(trans.press(13), 0xE0)
        self.assertEqual(trans.press(13), 0xE0)
        with self.assertRaisesRegex(RuntimeError, "release captured keys"): trans.replace(b)
        self.assertEqual(trans.release(13), 0xE0)
        self.assertIsNone(trans.release(13))
        trans.replace(b)
        self.assertEqual(trans.press(24), 0x04)
        self.assertEqual(trans.release_all(), (0x04,))
        self.assertEqual(trans.release_all(), ())
    def test_unsupported_names_and_malformed_codes_are_not_guessed(self):
        keymap = KeyMap.from_names({45: b"FK01", 46: b"LSGT", 47: b"HENK"})
        self.assertEqual(keymap.to_hid(45), 0x3A)
        self.assertEqual(keymap.to_hid(46), 0x64)
        self.assertIsNone(keymap.to_hid(47))
        for code in (7, 256, True):
            with self.assertRaises(ValueError): KeyMap.from_names({code: "LCTL"})


if __name__ == "__main__": unittest.main()
