"""Translate XKB physical names to USB HID keyboard-page usages.

X11 keycodes are server-specific and are never inferred from evdev offsets.
The caller obtains a fresh code->XKB-name snapshot from the actual X server and
must refresh it when the keymap changes. Unknown and ambiguous names are omitted.
"""
from collections import defaultdict
from dataclasses import dataclass


def _make_table():
    table = {}
    for prefix, letters in (("AD", "qwertyuiop"), ("AC", "asdfghjkl"), ("AB", "zxcvbnm")):
        for index, letter in enumerate(letters, 1):
            table[f"{prefix}{index:02}"] = ord(letter) - ord("a") + 0x04
    for index in range(1, 11):
        table[f"AE{index:02}"] = 0x1D + index
    table.update({
        "AE11": 0x2D, "AE12": 0x2E, "AD11": 0x2F, "AD12": 0x30,
        "AC10": 0x33, "AC11": 0x34, "AB08": 0x36, "AB09": 0x37, "AB10": 0x38,
        "RTRN": 0x28, "ESC": 0x29, "BKSP": 0x2A, "TAB": 0x2B, "SPCE": 0x2C,
        "BKSL": 0x31, "TLDE": 0x35, "CAPS": 0x39, "PRSC": 0x46, "SCLK": 0x47,
        "PAUS": 0x48, "INS": 0x49, "HOME": 0x4A, "PGUP": 0x4B, "DELE": 0x4C,
        "END": 0x4D, "PGDN": 0x4E, "RGHT": 0x4F, "LEFT": 0x50, "DOWN": 0x51,
        "UP": 0x52, "NMLK": 0x53, "KPDV": 0x54, "KPMU": 0x55, "KPSU": 0x56,
        "KPAD": 0x57, "KPEN": 0x58, "KP1": 0x59, "KP2": 0x5A, "KP3": 0x5B,
        "KP4": 0x5C, "KP5": 0x5D, "KP6": 0x5E, "KP7": 0x5F, "KP8": 0x60,
        "KP9": 0x61, "KP0": 0x62, "KPDL": 0x63, "LSGT": 0x64, "COMP": 0x65,
        "KPEQ": 0x67, "LCTL": 0xE0, "LFSH": 0xE1, "LALT": 0xE2, "LWIN": 0xE3,
        "RCTL": 0xE4, "RTSH": 0xE5, "RALT": 0xE6, "RWIN": 0xE7,
    })
    for index in range(1, 13): table[f"FK{index:02}"] = 0x39 + index
    for index in range(13, 25): table[f"FK{index:02}"] = 0x68 + index - 13
    return table


XKB_TO_HID = _make_table()


@dataclass(frozen=True)
class KeyMap:
    """Unique mappings only; unsupported or ambiguous keys have no entry."""
    forward: dict
    reverse: dict
    ambiguous: frozenset
    unsupported: frozenset

    @classmethod
    def from_names(cls, names):
        candidates = defaultdict(list)
        unsupported = set()
        for code, raw in names.items():
            if type(code) is not int or not 8 <= code <= 255:
                raise ValueError("X11 keycode outside server range")
            if isinstance(raw, bytes):
                try: name = raw.rstrip(b"\0 ").decode("ascii")
                except UnicodeDecodeError: name = ""
            elif isinstance(raw, str):
                name = raw.rstrip("\0 ")
            else:
                raise ValueError("invalid XKB key name")
            usage = XKB_TO_HID.get(name)
            if usage is None: unsupported.add(code)
            else: candidates[usage].append(code)
        ambiguous = frozenset(usage for usage, codes in candidates.items() if len(codes) != 1)
        reverse = {usage: codes[0] for usage, codes in candidates.items() if len(codes) == 1}
        return cls({code: usage for usage, code in reverse.items()}, reverse, ambiguous, frozenset(unsupported))

    def to_hid(self, code): return self.forward.get(code)
    def to_x11(self, usage): return self.reverse.get(usage)


class HeldKeyTranslator:
    """Preserve the keydown identity through keymap changes and repeated downs."""
    def __init__(self, keymap):
        self.keymap = keymap
        self.held = {}

    def press(self, keycode):
        if keycode in self.held: return self.held[keycode]
        usage = self.keymap.to_hid(keycode)
        if usage is not None: self.held[keycode] = usage
        return usage

    def release(self, keycode): return self.held.pop(keycode, None)

    def replace(self, keymap):
        if self.held:
            raise RuntimeError("release captured keys before changing the keymap")
        self.keymap = keymap

    def release_all(self):
        usages = tuple(self.held.values())
        self.held.clear()
        return usages
