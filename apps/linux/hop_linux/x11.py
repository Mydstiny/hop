"""Explicitly activated X11 input primitives for Hop.

No display is opened at import time. This module does not provide authentication,
networking, focus arbitration, physical-key translation or a runnable Hop UI.
It must not be hooked into an untrusted connection. All methods run on the
creating thread; the controller must release local grabs before network teardown.
"""
from __future__ import annotations

import ctypes as C
import ctypes.util
import os
import threading
from dataclasses import dataclass

# Xlib event types and masks, from X11/X.h.
KEY_PRESS, KEY_RELEASE, BUTTON_PRESS, BUTTON_RELEASE, MOTION = 2, 3, 4, 5, 6
BUTTON_MASK = (1 << 2) | (1 << 3) | (1 << 6)
GRAB_SUCCESS, GRAB_ASYNC, CURRENT_TIME = 0, 1, 0
_threading_initialized = False

def initialize_xlib_threads():
    """Call as the first Xlib operation in this process, before GTK/X11."""
    global _threading_initialized
    if _threading_initialized:return
    name=ctypes.util.find_library('X11')
    if not name:raise RuntimeError('X11 runtime library unavailable')
    lib=C.CDLL(name);lib.XInitThreads.argtypes=[];lib.XInitThreads.restype=C.c_int
    if not lib.XInitThreads():raise RuntimeError('XInitThreads failed')
    _threading_initialized=True

def xlib_threads_initialized():return _threading_initialized


class _Common(C.Structure):
    _fields_ = [("type", C.c_int), ("serial", C.c_ulong), ("send_event", C.c_int),
                ("display", C.c_void_p), ("window", C.c_ulong), ("root", C.c_ulong),
                ("subwindow", C.c_ulong), ("time", C.c_ulong), ("x", C.c_int),
                ("y", C.c_int), ("x_root", C.c_int), ("y_root", C.c_int),
                ("state", C.c_uint)]


class _Key(C.Structure):
    _fields_ = _Common._fields_ + [("keycode", C.c_uint), ("same_screen", C.c_int)]


class _Button(C.Structure):
    _fields_ = _Common._fields_ + [("button", C.c_uint), ("same_screen", C.c_int)]


class _Motion(C.Structure):
    _fields_ = _Common._fields_ + [("is_hint", C.c_char), ("same_screen", C.c_int)]


class _Event(C.Union):
    _fields_ = [("type", C.c_int), ("key", _Key), ("button", _Button),
                ("motion", _Motion), ("pad", C.c_long * 24)]


class _XkbKeyName(C.Structure):
    _fields_ = [("name", C.c_char * 4)]


class _XkbNames(C.Structure):
    _fields_ = [("keycodes", C.c_ulong), ("geometry", C.c_ulong), ("symbols", C.c_ulong),
                ("types", C.c_ulong), ("compat", C.c_ulong), ("vmods", C.c_ulong * 16),
                ("indicators", C.c_ulong * 32), ("groups", C.c_ulong * 4),
                ("keys", C.POINTER(_XkbKeyName)), ("key_aliases", C.c_void_p),
                ("radio_groups", C.c_void_p), ("phys_symbols", C.c_ulong),
                ("num_keys", C.c_ubyte), ("num_key_aliases", C.c_ubyte), ("num_rg", C.c_ushort)]


class _XkbDesc(C.Structure):
    _fields_ = [("display", C.c_void_p), ("flags", C.c_ushort), ("device_spec", C.c_ushort),
                ("min_key_code", C.c_ubyte), ("max_key_code", C.c_ubyte),
                ("ctrls", C.c_void_p), ("server", C.c_void_p), ("map", C.c_void_p),
                ("indicators", C.c_void_p), ("names", C.POINTER(_XkbNames)),
                ("compat", C.c_void_p), ("geom", C.c_void_p)]

class _XIAnyClass(C.Structure):
    _fields_ = [("type", C.c_int), ("sourceid", C.c_int)]

class _XIButtonState(C.Structure):
    _fields_ = [("mask_len", C.c_int), ("mask", C.POINTER(C.c_ubyte))]

class _XIButtonClass(C.Structure):
    _fields_ = [("type", C.c_int), ("sourceid", C.c_int), ("num_buttons", C.c_int),
                ("labels", C.POINTER(C.c_ulong)), ("state", _XIButtonState)]

class _XIDeviceInfo(C.Structure):
    _fields_ = [("deviceid", C.c_int), ("name", C.c_char_p), ("use", C.c_int),
                ("attachment", C.c_int), ("enabled", C.c_int), ("num_classes", C.c_int),
                ("classes", C.POINTER(C.POINTER(_XIAnyClass)))]

class _RRMonitor(C.Structure):
    _fields_ = [("name", C.c_ulong), ("primary", C.c_int), ("automatic", C.c_int),
                ("noutput", C.c_int), ("x", C.c_int), ("y", C.c_int),
                ("width", C.c_int), ("height", C.c_int), ("mwidth", C.c_int),
                ("mheight", C.c_int), ("outputs", C.POINTER(C.c_ulong))]


@dataclass(frozen=True)
class Event:
    kind: str
    code: int = 0
    down: bool = False
    x: int = 0
    y: int = 0
    state: int = 0


def _bind(library, name, restype, *args):
    func = getattr(library, name)
    func.restype = restype
    func.argtypes = list(args)
    return func


class _Native:
    def __init__(self):
        x11 = ctypes.util.find_library("X11")
        xtst = ctypes.util.find_library("Xtst")
        if not x11 or not xtst:
            raise RuntimeError("X11 and XTest runtime libraries are required")
        self.x = C.CDLL(x11)
        self.xt = C.CDLL(xtst)
        P, W, I, U, T = C.c_void_p, C.c_ulong, C.c_int, C.c_uint, C.c_ulong
        _bind(self.x, "XOpenDisplay", P, C.c_char_p)
        _bind(self.x, "XCloseDisplay", I, P)
        _bind(self.x, "XDefaultRootWindow", W, P)
        _bind(self.x, "XGrabKeyboard", I, P, W, I, I, I, T)
        _bind(self.x, "XGrabPointer", I, P, W, I, U, I, I, W, W, T)
        _bind(self.x, "XUngrabKeyboard", I, P, T)
        _bind(self.x, "XUngrabPointer", I, P, T)
        _bind(self.x, "XFlush", I, P)
        _bind(self.x, "XPending", I, P)
        _bind(self.x, "XNextEvent", I, P, C.POINTER(_Event))
        _bind(self.x, "XWarpPointer", I, P, W, W, I, I, U, U, I, I)
        _bind(self.x, "XQueryKeymap", I, P, C.POINTER(C.c_char))
        _bind(self.x, "XQueryPointer", I, P, W, C.POINTER(W), C.POINTER(W), C.POINTER(I), C.POINTER(I), C.POINTER(I), C.POINTER(I), C.POINTER(U))
        _bind(self.x, "XkbGetMap", C.POINTER(_XkbDesc), P, U, U)
        _bind(self.x, "XkbGetNames", I, P, U, C.POINTER(_XkbDesc))
        _bind(self.x, "XkbFreeKeyboard", None, C.POINTER(_XkbDesc), U, I)
        _bind(self.xt, "XTestQueryExtension", I, P, C.POINTER(I), C.POINTER(I), C.POINTER(I), C.POINTER(I))
        _bind(self.xt, "XTestFakeKeyEvent", I, P, U, I, T)
        _bind(self.xt, "XTestFakeButtonEvent", I, P, U, I, T)
        _bind(self.xt, "XTestFakeMotionEvent", I, P, I, I, I, T)

    def open(self):
        display = self.x.XOpenDisplay(None)
        if not display:
            raise RuntimeError("Cannot open X11 display; check DISPLAY and session permissions")
        return display

    def xtest_supported(self, display):
        e, r, major, minor = (C.c_int() for _ in range(4))
        return bool(self.xt.XTestQueryExtension(display, C.byref(e), C.byref(r), C.byref(major), C.byref(minor)))

    def root(self, display): return self.x.XDefaultRootWindow(display)
    def key_names(self, display):
        # XkbGetMap fetches the server's min/max keycodes. XkbGetNames then
        # requests physical key names; no fixed evdev offset is assumed.
        desc = self.x.XkbGetMap(display, 1, 0x100)  # KeyTypesMask, UseCoreKbd
        if not desc:
            raise RuntimeError("XKB keyboard description unavailable")
        try:
            if self.x.XkbGetNames(display, 1 << 9, desc) != 0:  # KeyNamesMask; Success = 0
                raise RuntimeError("XKB physical key names unavailable")
            data = desc.contents
            if not data.names or not data.names.contents.keys or data.min_key_code < 8 or data.max_key_code < data.min_key_code:
                raise RuntimeError("XKB returned invalid key names")
            keys = data.names.contents.keys
            return {code: bytes(keys[code].name) for code in range(data.min_key_code, data.max_key_code + 1)}
        finally:
            self.x.XkbFreeKeyboard(desc, 0x7F, True)  # AllComponentsMask
    def grab_keyboard(self, display, root):
        return self.x.XGrabKeyboard(display, root, False, GRAB_ASYNC, GRAB_ASYNC, CURRENT_TIME)
    def grab_pointer(self, display, root):
        return self.x.XGrabPointer(display, root, False, BUTTON_MASK, GRAB_ASYNC, GRAB_ASYNC, 0, 0, CURRENT_TIME)
    def ungrab_keyboard(self, display): self.x.XUngrabKeyboard(display, CURRENT_TIME)
    def ungrab_pointer(self, display): self.x.XUngrabPointer(display, CURRENT_TIME)
    def flush(self, display): self.x.XFlush(display)
    def close(self, display): self.x.XCloseDisplay(display)
    def key(self, display, code, down): return bool(self.xt.XTestFakeKeyEvent(display, code, down, 0))
    def button(self, display, code, down): return bool(self.xt.XTestFakeButtonEvent(display, code, down, 0))
    def motion(self, display, x, y): return bool(self.xt.XTestFakeMotionEvent(display, -1, x, y, 0))
    def warp(self, display, root, x, y): self.x.XWarpPointer(display, 0, root, 0, 0, 0, 0, x, y)
    def screens(self, display, root):
        P,W,I=C.c_void_p,C.c_ulong,C.c_int
        name=ctypes.util.find_library('Xrandr')
        if not name:raise RuntimeError('Xrandr is required for monitor enumeration')
        rr=C.CDLL(name)
        _bind(rr,'XRRQueryVersion',I,P,C.POINTER(I),C.POINTER(I))
        _bind(rr,'XRRGetMonitors',C.POINTER(_RRMonitor),P,W,I,C.POINTER(I))
        _bind(rr,'XRRFreeMonitors',None,C.POINTER(_RRMonitor))
        major,minor=I(),I()
        if not rr.XRRQueryVersion(display,C.byref(major),C.byref(minor)) or (major.value,minor.value)<(1,5):
            raise RuntimeError('RandR 1.5 monitor enumeration unavailable')
        count=I();monitors=rr.XRRGetMonitors(display,root,True,C.byref(count))
        if not monitors:raise RuntimeError('monitor enumeration failed')
        try:
            if not 1<=count.value<=64:raise RuntimeError('invalid monitor count')
            result=[];ids=set()
            for index in range(count.value):
                m=monitors[index];screen=(int(m.name),m.x,m.y,m.width,m.height)
                if (not screen[0] or screen[0] in ids or m.x<0 or m.y<0 or m.width<=0 or m.height<=0 or
                    m.x+m.width>32768 or m.y+m.height>32768):raise RuntimeError('unsupported monitor geometry')
                ids.add(screen[0]);result.append(screen)
            return tuple(result)
        finally:rr.XRRFreeMonitors(monitors)
    def input_state_clear(self, display, root):
        keys = (C.c_char * 32)()
        self.x.XQueryKeymap(display, keys)
        root_out, child = C.c_ulong(), C.c_ulong()
        rx, ry, wx, wy = (C.c_int() for _ in range(4)); mask = C.c_uint()
        if not self.x.XQueryPointer(display, root, C.byref(root_out), C.byref(child), C.byref(rx), C.byref(ry), C.byref(wx), C.byref(wy), C.byref(mask)):
            raise RuntimeError("cannot query X11 pointer state")
        # ENTER does not yet synchronize latched Caps/Shift lock state.
        if any(keys.raw) or mask.value & ((0x1f << 8) | (1 << 1)): return False
        # XI2 includes side buttons and attached devices absent from the core
        # pointer mask. Refuse capture when the extension cannot be checked.
        name = ctypes.util.find_library("Xi")
        if not name: raise RuntimeError("XI2 is required to check held buttons")
        xi = C.CDLL(name)
        _bind(xi, "XIQueryVersion", C.c_int, C.c_void_p, C.POINTER(C.c_int), C.POINTER(C.c_int))
        _bind(xi, "XIQueryDevice", C.POINTER(_XIDeviceInfo), C.c_void_p, C.c_int, C.POINTER(C.c_int))
        _bind(xi, "XIFreeDeviceInfo", None, C.POINTER(_XIDeviceInfo))
        major, minor = C.c_int(2), C.c_int(0)
        if xi.XIQueryVersion(display, C.byref(major), C.byref(minor)) != 0 or major.value < 2:
            raise RuntimeError("XI2 is unavailable")
        count = C.c_int()
        devices = xi.XIQueryDevice(display, 0, C.byref(count)) # XIAllDevices
        if not devices: raise RuntimeError("XI2 device query failed")
        try:
            if not 1 <= count.value <= 1024: raise RuntimeError("invalid XI2 device count")
            found = False
            for i in range(count.value):
                device=devices[i]
                if not device.enabled: continue
                if not 0 <= device.num_classes <= 1024 or (device.num_classes and not device.classes):
                    raise RuntimeError("invalid XI2 classes")
                for j in range(device.num_classes):
                    info=device.classes[j]
                    if not info: raise RuntimeError("invalid XI2 class")
                    if info.contents.type != 1: continue # XIButtonClass
                    button=C.cast(info,C.POINTER(_XIButtonClass)).contents
                    if not 0 <= button.state.mask_len <= 1024 or (button.state.mask_len and not button.state.mask):
                        raise RuntimeError("invalid XI2 button mask")
                    found=True
                    if any(button.state.mask[k] for k in range(button.state.mask_len)): return False
            if not found: raise RuntimeError("XI2 pointer state unavailable")
            return True
        finally: xi.XIFreeDeviceInfo(devices)

    def pointer_position(self, display, root):
        root_out, child = C.c_ulong(), C.c_ulong()
        rx, ry, wx, wy = (C.c_int() for _ in range(4)); mask = C.c_uint()
        if not self.x.XQueryPointer(display, root, C.byref(root_out), C.byref(child), C.byref(rx), C.byref(ry), C.byref(wx), C.byref(wy), C.byref(mask)):
            raise RuntimeError('cannot query X11 pointer position')
        return rx.value, ry.value
    def events(self, display, limit):
        result = []
        while len(result) < limit and self.x.XPending(display):
            raw = _Event()
            self.x.XNextEvent(display, C.byref(raw))
            if raw.type in (KEY_PRESS, KEY_RELEASE):
                result.append(Event("key", raw.key.keycode, raw.type == KEY_PRESS, state=raw.key.state))
            elif raw.type in (BUTTON_PRESS, BUTTON_RELEASE):
                result.append(Event("button", raw.button.button, raw.type == BUTTON_PRESS,
                                    raw.button.x_root, raw.button.y_root, raw.button.state))
            elif raw.type == MOTION:
                result.append(Event("motion", x=raw.motion.x_root, y=raw.motion.y_root, state=raw.motion.state))
        return result


class X11Backend:
    """Low-level grab/injection; constructing it opens the user's X11 display.

    Capture is *not* started until grab() is explicitly called. Caller owns
    authentication, a timeout/escape path, key identity mapping and event routing.
    X11 grabs must never be held while waiting for network or UI operations.
    """
    def __init__(self, native=None):
        self._owner = threading.get_ident()
        if native is None and (os.environ.get("WAYLAND_DISPLAY") or os.environ.get("XDG_SESSION_TYPE", "").lower() == "wayland"):
            raise RuntimeError("XWayland cannot provide desktop-wide capture; use a supported Wayland portal backend")
        self._native = native if native is not None else _Native()
        self._display = self._native.open()
        self._grabbed = False
        self._keys = set()
        self._hid_down = {}
        self._buttons = set()
        try:
            if not self._native.xtest_supported(self._display):
                raise RuntimeError("X server does not support XTest")
            self._root = self._native.root(self._display)
        except BaseException:
            self._native.close(self._display)
            self._display = None
            raise

    def _check(self):
        if threading.get_ident() != self._owner:
            raise RuntimeError("X11 backend must be used on its owning thread")
        if self._display is None:
            raise RuntimeError("X11 backend is closed")

    def grab(self):
        self._check()
        if self._grabbed:
            return
        if self._native.grab_keyboard(self._display, self._root) != GRAB_SUCCESS:
            raise RuntimeError("X11 keyboard grab denied")
        try:
            if self._native.grab_pointer(self._display, self._root) != GRAB_SUCCESS:
                raise RuntimeError("X11 pointer grab denied")
        except BaseException:
            self._native.ungrab_keyboard(self._display)
            self._native.flush(self._display)
            raise
        self._grabbed = True
        self._native.flush(self._display)

    def ungrab(self):
        self._check()
        if not self._grabbed:
            return
        # Both releases precede any network operation; X server connection close
        # is the final fallback if XUngrab raises or the process exits.
        try:
            self._native.ungrab_pointer(self._display)
        finally:
            try:
                self._native.ungrab_keyboard(self._display)
            finally:
                self._grabbed = False
                self._native.flush(self._display)

    def events(self, limit=256):
        self._check()
        if type(limit) is not int or not 1 <= limit <= 1024:
            raise ValueError("event limit must be between 1 and 1024")
        if not self._grabbed:
            return []
        return self._native.events(self._display, limit)

    def input_state_clear(self):
        self._check()
        return self._native.input_state_clear(self._display, self._root)

    def pointer_position(self):
        """Read the pointer position without grabbing or injecting input."""
        self._check()
        return self._native.pointer_position(self._display, self._root)

    def screens(self):
        """Enumerate active RandR monitors without capturing or injecting input."""
        self._check()
        return self._native.screens(self._display, self._root)

    def keymap(self):
        """Read the current XKB physical names without listening to key events."""
        from .keymap import KeyMap
        self._check()
        return KeyMap.from_names(self._native.key_names(self._display))

    def key(self, keycode, down):
        self._check()
        if type(keycode) is not int or not 8 <= keycode <= 255 or not isinstance(down, bool):
            raise ValueError("invalid X11 keycode or action")
        if not self._native.key(self._display, keycode, down):
            raise RuntimeError("XTest key injection failed")
        if down: self._keys.add(keycode)
        else: self._keys.discard(keycode)
        self._native.flush(self._display)

    def button(self, button, down):
        self._check()
        if type(button) is not int or button not in (1, 2, 3, 8, 9) or not isinstance(down, bool):
            raise ValueError("unsupported X11 mouse button or action")
        if not self._native.button(self._display, button, down):
            raise RuntimeError("XTest button injection failed")
        if down: self._buttons.add(button)
        else: self._buttons.discard(button)
        self._native.flush(self._display)

    def key_hid(self, keymap, usage, down):
        """Inject a mapped HID usage. Release uses the original pressed keycode."""
        self._check()
        if type(usage) is not int or not isinstance(down, bool):
            raise ValueError("invalid HID key action")
        if down:
            code = self._hid_down.get(usage)
            if code is None:
                code = keymap.to_x11(usage)
            if code is None:
                raise ValueError("HID usage is missing or ambiguous in the current XKB keymap")
            self.key(code, True)
            self._hid_down[usage] = code
            return True
        code = self._hid_down.get(usage)
        if code is None:
            return False
        self.key(code, False)
        del self._hid_down[usage]
        return True

    def motion(self, x, y):
        self._check()
        if not all(type(v) is int and 0 <= v <= 32767 for v in (x, y)):
            raise ValueError("invalid X11 absolute pointer position")
        if not self._native.motion(self._display, x, y):
            raise RuntimeError("XTest motion injection failed")
        self._native.flush(self._display)

    def warp(self, x, y):
        """Recenter the captured pointer; caller filters the resulting event."""
        self._check()
        if not self._grabbed or not all(type(v) is int and 0 <= v <= 32767 for v in (x,y)):
            raise ValueError("invalid capture warp")
        self._native.warp(self._display, self._root, x, y)
        self._native.flush(self._display)

    def scroll(self, horizontal_steps, vertical_steps):
        """Inject discrete wheel notches; positive x is right, positive y is down.

        XTest core-button emulation does not preserve touchpad inertia or
        high-resolution axis data. Bound a single call so it cannot monopolize
        the input path or exhaust memory.
        """
        self._check()
        if not all(type(v) is int and abs(v) <= 100 for v in (horizontal_steps, vertical_steps)):
            raise ValueError("wheel steps must be integers between -100 and 100")
        for steps, positive, negative in ((horizontal_steps, 7, 6), (vertical_steps, 5, 4)):
            code = positive if steps > 0 else negative
            for _ in range(abs(steps)):
                if not self._native.button(self._display, code, True):
                    raise RuntimeError("XTest wheel press failed")
                self._buttons.add(code)
                if not self._native.button(self._display, code, False):
                    self._native.flush(self._display)
                    raise RuntimeError("XTest wheel release failed")
                self._buttons.discard(code)
        self._native.flush(self._display)

    def release_all(self):
        self._check()
        # Retain failed releases so a later cleanup can retry them.
        failures = []
        for code in sorted(self._keys):
            try:
                if self._native.key(self._display, code, False): self._keys.discard(code)
                else: failures.append(("key", code))
            except Exception:
                failures.append(("key", code))
        for code in sorted(self._buttons):
            try:
                if self._native.button(self._display, code, False): self._buttons.discard(code)
                else: failures.append(("button", code))
            except Exception:
                failures.append(("button", code))
        self._hid_down = {usage: code for usage, code in self._hid_down.items() if code in self._keys}
        self._native.flush(self._display)
        return failures

    def close(self):
        if self._display is None:
            return
        self._check()
        failures = []
        try:
            self.ungrab()
        except Exception:
            failures.append(("ungrab", 0))
        try:
            failures.extend(self.release_all())
        except Exception:
            failures.append(("release", 0))
        finally:
            self._native.close(self._display)
            self._display = None
        if failures:
            raise RuntimeError("X11 cleanup failed: " + repr(failures))

    def __enter__(self): return self
    def __exit__(self, *_): self.close()
