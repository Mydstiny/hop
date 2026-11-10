"""Bounded, focus-gated X11 clipboard selection transfer state.

No X server calls or networking are made here. A future Xlib driver must wire
these decisions to SelectionRequest/SelectionNotify/PropertyNotify events,
keep the selection-owner window alive, and verify server operations. This module
is not a complete system clipboard or transport implementation.
"""
from __future__ import annotations

from dataclasses import dataclass
import struct
import time
import zlib

TEXT = "UTF8_STRING"
PNG = "image/png"
MAX_TEXT = 2 * 1024 * 1024
MAX_PNG = 16 * 1024 * 1024
MAX_PIXELS = 4096 * 4096
MAX_DIMENSION = 16384
MAX_PNG_CHUNKS = 4096
CHUNK = 64 * 1024
MAX_TRANSFERS = 4
TIMEOUT = 10.0


def validate_payload(target, data):
    if not isinstance(data, bytes):
        raise ValueError("clipboard payload must be bytes")
    if target == TEXT:
        if len(data) > MAX_TEXT: raise ValueError("text clipboard exceeds limit")
        try: data.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc: raise ValueError("invalid UTF-8 clipboard") from exc
        return
    if target != PNG or len(data) > MAX_PNG:
        raise ValueError("unsupported or oversized clipboard format")
    # Structural validation only. A real decoder must independently validate
    # PNG/zlib bytes and enforce the same memory and pixel limits.
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("invalid PNG signature")
    offset, seen_header, seen_data, seen_end, chunks = 8, False, False, False, 0
    while offset + 12 <= len(data):
        chunks += 1
        if chunks > MAX_PNG_CHUNKS: raise ValueError("too many PNG chunks")
        length = struct.unpack_from(">I", data, offset)[0]
        if length > MAX_PNG or offset + 12 + length > len(data):
            raise ValueError("invalid PNG chunk bounds")
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + length]
        crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        if zlib.crc32(payload, zlib.crc32(kind)) & 0xFFFFFFFF != crc:
            raise ValueError("invalid PNG chunk CRC")
        if not seen_header:
            if kind != b"IHDR" or length != 13: raise ValueError("invalid PNG IHDR")
            width, height = struct.unpack_from(">II", payload)
            if not width or not height or width > MAX_DIMENSION or height > MAX_DIMENSION or width * height > MAX_PIXELS:
                raise ValueError("PNG dimensions exceed limit")
            seen_header = True
        elif kind == b"IHDR":
            raise ValueError("duplicate PNG IHDR")
        if kind == b"IDAT": seen_data = True
        if kind == b"IEND":
            if length or not seen_data or offset + 12 != len(data):
                raise ValueError("invalid PNG IEND")
            seen_end = True
            break
        offset += 12 + length
    if not seen_end: raise ValueError("incomplete PNG")


def _limit(target):
    if target == TEXT: return MAX_TEXT
    if target == PNG: return MAX_PNG
    raise ValueError("unsupported selection target")


@dataclass(frozen=True)
class Focus:
    peer: str
    epoch: int
    enabled: bool = True

    def permits(self, peer, epoch):
        return bool(self.enabled is True and type(self.peer) is str and self.peer and
                    type(self.epoch) is int and self.epoch > 0 and
                    type(epoch) is int and peer == self.peer and epoch == self.epoch)


class SelectionReceiver:
    """One non-blocking X11 selection read including ICCCM INCR chunks.

    The caller performs XConvertSelection, reads properties and deletes them
    after each chunk. The focus/epoch is checked on every callback. Timeout and
    unsupported/malformed data discard the buffer instead of exposing partial
    clipboard content. INCR's declared size is only a lower bound.
    """
    def __init__(self, focus, now=time.monotonic):
        self.focus = focus
        self.now = now
        self._state = None
        self._generation = 0

    def cancel(self): self._state = None

    def begin(self, peer, epoch, target):
        if not self.focus.permits(peer, epoch): raise PermissionError("inactive clipboard peer")
        limit = _limit(target)
        self._generation += 1
        self._state = {"peer": peer, "epoch": epoch, "target": target, "token": self._generation,
                       "limit": limit, "deadline": self.now() + TIMEOUT,
                       "data": bytearray(), "incr": False}
        return self._generation

    def _current(self, token):
        s = self._state
        if not s: raise RuntimeError("no active selection request")
        if token != s["token"]: raise RuntimeError("stale selection callback")
        if not self.focus.permits(s["peer"], s["epoch"]) or self.now() >= s["deadline"]:
            self.cancel()
            raise RuntimeError("selection expired or focus changed")
        return s

    def select_target(self,token,target):
        """Choose an advertised target before reading any selection bytes."""
        state=self._current(token)
        if state['incr'] or state['data']:raise RuntimeError('selection read already started')
        state['target']=target;state['limit']=_limit(target)

    def initial(self, token, property_type, format_bits, value):
        s = self._current(token)
        if s["incr"]:
            self.cancel(); raise ValueError("duplicate selection response")
        if property_type == "INCR":
            if format_bits != 32 or type(value) is not int or value < 0 or value > s["limit"]:
                self.cancel(); raise ValueError("invalid INCR size or format")
            s["incr"] = True
            return None
        if property_type != s["target"] or format_bits != 8 or not isinstance(value, bytes):
            self.cancel(); raise ValueError("invalid selection property")
        self.cancel(); validate_payload(s["target"], value)
        return value

    def chunk(self, token, property_type, format_bits, data):
        s = self._current(token)
        if not s["incr"] or property_type != s["target"] or format_bits != 8 or not isinstance(data, bytes):
            self.cancel(); raise ValueError("invalid INCR chunk")
        if len(s["data"]) + len(data) > s["limit"]:
            self.cancel(); raise ValueError("INCR clipboard exceeds limit")
        if not data:
            payload = bytes(s["data"]); target = s["target"]
            self.cancel(); validate_payload(target, payload)
            return payload
        s["data"].extend(data)
        return None


class SelectionOwner:
    """Serve locally owned clipboard content with bounded INCR transfers.

    The Xlib owner still must take and verify selection ownership, advertise
    TARGETS, select PropertyChangeMask on each requestor, send SelectionNotify,
    and act on PropertyNotify/Delete. These methods perform no X11 calls.
    """
    def __init__(self, focus, now=time.monotonic):
        self.focus, self.now = focus, now
        self.target = None
        self.data = b""
        self.transfers = {}

    def install_remote(self, peer, epoch, target, data):
        if not self.focus.permits(peer, epoch): raise PermissionError("inactive clipboard peer")
        validate_payload(target, data)
        self.target, self.data = target, data

    def selection_lost(self):
        self.target = None
        self.data = b""
        # Already accepted INCR requests retain an immutable snapshot; losing
        # selection ownership must not strand another client's paste request.

    def request(self, requestor, property_name, target):
        if self.target is None: return None
        if not requestor or not property_name: raise ValueError("missing requestor or property")
        if target == "TARGETS": return ("ATOM", ("TARGETS", self.target))
        if target != self.target: return None
        if len(self.data) <= CHUNK: return (self.target, self.data)
        self.expire()
        key = (requestor, property_name)
        if key in self.transfers or len(self.transfers) >= MAX_TRANSFERS:
            return None
        # Snapshot stays owned until transfer completes or ownership is lost.
        self.transfers[key] = {"offset": 0, "deadline": self.now() + TIMEOUT,
                               "target": self.target, "data": self.data}
        return ("INCR", len(self.data))

    def property_deleted(self, requestor, property_name):
        self.expire()
        key = (requestor, property_name)
        entry = self.transfers.get(key)
        if entry is None: return None
        offset = entry["offset"]
        if offset >= len(entry["data"]):
            del self.transfers[key]
            return (entry["target"], b"")
        payload = entry["data"][offset:offset + CHUNK]
        entry["offset"] += len(payload)
        entry["deadline"] = self.now() + TIMEOUT
        return (entry["target"], payload)

    def expire(self):
        now = self.now()
        for key in list(self.transfers):
            if now >= self.transfers[key]["deadline"]: del self.transfers[key]
