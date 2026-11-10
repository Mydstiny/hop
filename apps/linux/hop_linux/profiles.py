"""Per-peer software pointer/scroll preferences; never changes hardware DPI."""
from __future__ import annotations

from dataclasses import asdict, dataclass
import fcntl
import json
import math
import os
from pathlib import Path
import re
import tempfile

_MAX_FILE = 1024 * 1024
_MAX_PROFILES = 256


def _number(value):
    return type(value) in (int, float) and math.isfinite(value) and .25 <= value <= 4


def _peer(value):
    # Device identity must come from an authenticated certificate fingerprint,
    # not a mutable hostname or discovery advertisement.
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", value):
        raise ValueError("peer identity must be a verified SHA-256 fingerprint")
    return value.lower()


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result: raise ValueError("duplicate JSON key")
        result[key] = value
    return result


@dataclass(frozen=True)
class PointerProfile:
    movement: float = 1.0
    horizontal_scroll: float = 1.0
    vertical_scroll: float = 1.0
    reverse_horizontal: bool = False
    reverse_vertical: bool = False
    forward_hz: int = 240

    def __post_init__(self):
        if (not all(_number(v) for v in (self.movement, self.horizontal_scroll, self.vertical_scroll)) or
            type(self.reverse_horizontal) is not bool or type(self.reverse_vertical) is not bool or
            type(self.forward_hz) is not int or self.forward_hz not in (60,120,240,500,1000)):
            raise ValueError("invalid pointer profile")

    def move(self, x, y):
        self._delta(x, y)
        return x * self.movement, y * self.movement

    def scroll(self, x, y):
        self._delta(x, y)
        return (x * self.horizontal_scroll * (-1 if self.reverse_horizontal else 1),
                y * self.vertical_scroll * (-1 if self.reverse_vertical else 1))

    @staticmethod
    def _delta(x, y):
        if not all(type(v) in (int, float) and math.isfinite(v) and abs(v) <= 1000000 for v in (x, y)):
            raise ValueError("invalid pointer delta")


class ProfileStore:
    """Atomic, locked updates to an explicitly selected per-user JSON file.

    No file is touched until load/update is explicitly called. A caller owns
    the path and supplies the active authenticated device fingerprint. The UI
    may say sensitivity or movement multiplier, never hardware DPI.
    """
    def __init__(self, path):
        self.path = Path(path)
        if not self.path.is_absolute() or self.path.name in ("", ".", ".."):
            raise ValueError("profile path must be an absolute file path")

    def _read(self):
        try:
            if self.path.is_symlink(): raise ValueError("profile file must not be a symlink")
            with self.path.open("rb") as stream:
                raw = stream.read(_MAX_FILE + 1)
        except FileNotFoundError:
            return {}
        if len(raw) > _MAX_FILE: raise ValueError("profile file exceeds limit")
        try:
            doc = json.loads(raw, object_pairs_hook=_unique_object)
            if not isinstance(doc, dict) or type(doc.get("version")) is not int or doc.get("version") != 1 or set(doc) != {"version", "peers"}:
                raise ValueError("invalid profile document")
            peers = doc["peers"]
            if not isinstance(peers, dict) or len(peers) > _MAX_PROFILES: raise ValueError("invalid profile count")
            result = {}
            for key, item in peers.items():
                canonical = _peer(key)
                if canonical in result or not isinstance(item, dict): raise ValueError("duplicate or invalid peer")
                fields={"movement", "horizontal_scroll", "vertical_scroll", "reverse_horizontal", "reverse_vertical"}
                if set(item) not in (fields,fields|{"forward_hz"}):
                    raise ValueError("invalid profile fields")
                result[canonical] = PointerProfile(**item)
            return result
        except (TypeError, ValueError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("invalid pointer profiles; existing file was preserved") from exc

    def get(self, peer):
        key = _peer(peer)
        return self._read().get(key, PointerProfile())

    def update(self, peer, profile):
        key = _peer(peer)
        if not isinstance(profile, PointerProfile): raise ValueError("pointer profile required")
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        lock_path = self.path.with_name(self.path.name + ".lock")
        flags = os.O_CREAT | os.O_RDWR
        if hasattr(os, "O_NOFOLLOW"): flags |= os.O_NOFOLLOW
        fd = os.open(lock_path, flags, 0o600)
        try:
            lock = os.fdopen(fd, "a+b")
        except BaseException:
            os.close(fd)
            raise
        with lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            profiles = self._read()
            if key not in profiles and len(profiles) >= _MAX_PROFILES: raise ValueError("too many peers")
            profiles[key] = profile
            payload = json.dumps({"version": 1, "peers": {k: asdict(v) for k, v in profiles.items()}},
                                 ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()
            if len(payload) > _MAX_FILE: raise ValueError("profile file exceeds limit")
            self._write(payload)

    def _write(self, payload):
        name = None
        try:
            fd, name = tempfile.mkstemp(prefix=".hop-profile-", dir=self.path.parent)
            with os.fdopen(fd, "wb") as stream:
                stream.write(payload); stream.flush(); os.fsync(stream.fileno())
            if self.path.is_symlink(): raise ValueError("profile file must not be a symlink")
            os.replace(name, self.path)
            name = None
        finally:
            if name is not None:
                try: os.unlink(name)
                except FileNotFoundError: pass
