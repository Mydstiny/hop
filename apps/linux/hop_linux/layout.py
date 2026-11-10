"""Bounded device/display layout and atomic local persistence.

This module has no discovery or session integration. Coordinates are logical
layout units, not physical dimensions. Edges connect only when screen rectangles
meet exactly and overlap by a positive length; corner-only contact never connects.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass, replace
import fcntl
import json
import os
from pathlib import Path
import re
import tempfile

MAX_DEVICES = 32
MAX_SCREENS = 8
MAX_DOCUMENT = 1024 * 1024
MAX_COORD = 100000
MAX_DIMENSION = 16384


def _integer(value, low, high): return type(value) is int and low <= value <= high


@dataclass(frozen=True)
class Screen:
    id: str
    x: int
    y: int
    width: int
    height: int

    def __post_init__(self):
        if (not isinstance(self.id, str) or not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", self.id) or
            not _integer(self.x, -MAX_COORD, MAX_COORD) or not _integer(self.y, -MAX_COORD, MAX_COORD) or
            not _integer(self.width, 1, MAX_DIMENSION) or not _integer(self.height, 1, MAX_DIMENSION)):
            raise ValueError("invalid screen")


@dataclass(frozen=True)
class Device:
    # Peer ID is the previously verified certificate fingerprint.
    id: str
    name: str
    system: str
    x: int
    y: int
    screens: tuple[Screen, ...]

    def __post_init__(self):
        if (not isinstance(self.id, str) or not re.fullmatch(r"[0-9a-f]{64}", self.id) or
            not isinstance(self.name, str) or not self.name.strip() or len(self.name) > 64 or
            any(ord(c) < 32 for c in self.name) or
            self.system not in ("macos", "windows", "linux", "harmonyos") or
            not _integer(self.x, -MAX_COORD, MAX_COORD) or not _integer(self.y, -MAX_COORD, MAX_COORD) or
            not isinstance(self.screens, tuple) or not 1 <= len(self.screens) <= MAX_SCREENS or
            not all(isinstance(screen, Screen) for screen in self.screens) or
            len({screen.id for screen in self.screens}) != len(self.screens)):
            raise ValueError("invalid device")
        for screen in self.screens:
            if abs(self.x + screen.x) > MAX_COORD or abs(self.y + screen.y) > MAX_COORD:
                raise ValueError("display position outside layout bounds")

    def rectangle(self, screen):
        return (self.x + screen.x, self.y + screen.y, screen.width, screen.height)

    def bounds(self):
        boxes = [self.rectangle(s) for s in self.screens]
        left = min(b[0] for b in boxes); top = min(b[1] for b in boxes)
        right = max(b[0] + b[2] for b in boxes); bottom = max(b[1] + b[3] for b in boxes)
        return (left, top, right - left, bottom - top)


@dataclass(frozen=True)
class Validation:
    errors: tuple[str, ...]
    warnings: tuple[str, ...]
    edges: tuple[tuple[str, str], ...]


def _overlap(a, b):
    return max(a[0], b[0]) < min(a[0] + a[2], b[0] + b[2]) and max(a[1], b[1]) < min(a[1] + a[3], b[1] + b[3])


def _touches(a, b):
    return (((a[0] + a[2] == b[0] or b[0] + b[2] == a[0]) and max(a[1], b[1]) < min(a[1] + a[3], b[1] + b[3])) or
            ((a[1] + a[3] == b[1] or b[1] + b[3] == a[1]) and max(a[0], b[0]) < min(a[0] + a[2], b[0] + b[2])))


class Layout:
    def __init__(self, devices=()):
        if not isinstance(devices, (tuple, list)) or len(devices) > MAX_DEVICES or not all(isinstance(d, Device) for d in devices):
            raise ValueError("invalid devices")
        self.devices = tuple(devices)
        if len({d.id for d in self.devices}) != len(self.devices): raise ValueError("duplicate device identity")

    def move(self, device_id, x, y):
        if not any(d.id == device_id for d in self.devices): raise ValueError("unknown device")
        return Layout(tuple(replace(d, x=x, y=y) if d.id == device_id else d for d in self.devices))

    def move_screen(self, device_id, screen_id, x, y):
        device = next((d for d in self.devices if d.id == device_id), None)
        if device is None or not any(s.id == screen_id for s in device.screens):
            raise ValueError("unknown device or screen")
        moved = replace(device, screens=tuple(replace(s, x=x, y=y) if s.id == screen_id else s for s in device.screens))
        return Layout(tuple(moved if d.id == device_id else d for d in self.devices))

    def validate(self):
        boxes = [(d.id, s.id, d.rectangle(s)) for d in self.devices for s in d.screens]
        errors, edges = [], set()
        for index, (a_id, a_screen, a) in enumerate(boxes):
            for b_id, b_screen, b in boxes[index + 1:]:
                if _overlap(a, b): errors.append(f"overlap:{a_id}:{a_screen}:{b_id}:{b_screen}")
                elif a_id != b_id and _touches(a, b): edges.add(tuple(sorted((a_id, b_id))))
        warnings = []
        if len(self.devices) > 1:
            seen = {self.devices[0].id}
            while True:
                expanded = seen | {b for a, b in edges if a in seen} | {a for a, b in edges if b in seen}
                if expanded == seen: break
                seen = expanded
            if len(seen) != len(self.devices): warnings.append("disconnected_devices")
        return Validation(tuple(errors), tuple(warnings), tuple(sorted(edges)))

    def to_data(self):
        devices = []
        for device in self.devices:
            item = asdict(device)
            item["screens"] = [asdict(screen) for screen in device.screens]
            devices.append(item)
        return {"version": 1, "devices": devices}

    @classmethod
    def from_data(cls, data):
        if not isinstance(data, dict) or set(data) != {"version", "devices"} or type(data["version"]) is not int or data["version"] != 1 or not isinstance(data["devices"], list):
            raise ValueError("invalid layout document")
        devices = []
        for raw in data["devices"]:
            if not isinstance(raw, dict) or set(raw) != {"id", "name", "system", "x", "y", "screens"} or not isinstance(raw["screens"], list):
                raise ValueError("invalid device document")
            screens = []
            for screen in raw["screens"]:
                if not isinstance(screen, dict) or set(screen) != {"id", "x", "y", "width", "height"}: raise ValueError("invalid screen document")
                screens.append(Screen(**screen))
            devices.append(Device(raw["id"], raw["name"], raw["system"], raw["x"], raw["y"], tuple(screens)))
        return cls(tuple(devices))


def sample_layout():
    return Layout((
        Device("a" * 64, "Mac", "macos", 0, 0, (Screen("mac-main", 0, 0, 900, 600),)),
        Device("b" * 64, "HarmonyOS", "harmonyos", 900, 0, (Screen("harmony-main", 0, 0, 800, 500), Screen("harmony-extra", 800, 0, 800, 500))),
        Device("c" * 64, "Windows", "windows", 900, 500, (Screen("windows-main", 0, 0, 800, 500),)),
    ))


def _unique(pairs):
    data = {}
    for key, value in pairs:
        if key in data: raise ValueError("duplicate JSON key")
        data[key] = value
    return data


class LayoutStore:
    """Caller chooses an absolute app-owned file path; never writes automatically."""
    def __init__(self, path):
        self.path = Path(path)
        if not self.path.is_absolute(): raise ValueError("layout path must be absolute")

    def load(self):
        if self.path.is_symlink(): raise ValueError("layout file must not be a symlink")
        with self.path.open("rb") as stream: raw = stream.read(MAX_DOCUMENT + 1)
        if len(raw) > MAX_DOCUMENT: raise ValueError("layout exceeds size limit")
        try:
            layout = Layout.from_data(json.loads(raw, object_pairs_hook=_unique))
            if layout.validate().errors: raise ValueError("overlapping screens")
            return layout
        except (TypeError, ValueError, UnicodeDecodeError) as exc:
            raise ValueError("invalid saved layout; existing file preserved") from exc

    def save(self, layout):
        if not isinstance(layout, Layout) or layout.validate().errors: raise ValueError("cannot save invalid layout")
        raw = json.dumps(layout.to_data(), ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode()
        if len(raw) > MAX_DOCUMENT: raise ValueError("layout exceeds size limit")
        self.path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
        lockpath = self.path.with_name(self.path.name + ".lock")
        flags = os.O_CREAT | os.O_RDWR | getattr(os, "O_NOFOLLOW", 0)
        fd = os.open(lockpath, flags, 0o600)
        try: lock = os.fdopen(fd, "a+b")
        except BaseException:
            os.close(fd); raise
        with lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            if self.path.is_symlink(): raise ValueError("layout file must not be a symlink")
            name = None
            try:
                fd, name = tempfile.mkstemp(prefix=".hop-layout-", dir=self.path.parent)
                with os.fdopen(fd, "wb") as stream:
                    stream.write(raw); stream.flush(); os.fsync(stream.fileno())
                if self.path.is_symlink(): raise ValueError("layout file must not be a symlink")
                os.replace(name, self.path); name = None
            finally:
                if name is not None:
                    try: os.unlink(name)
                    except FileNotFoundError: pass
