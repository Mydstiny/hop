"""Read-only local capability inventory; never opens a display or captures input."""
from __future__ import annotations

import ctypes.util
import os
import platform


def inventory(environ=None, find_library=None):
    env = os.environ if environ is None else environ
    find = ctypes.util.find_library if find_library is None else find_library
    wayland = bool(env.get("WAYLAND_DISPLAY")) or env.get("XDG_SESSION_TYPE", "").lower() == "wayland"
    x_display = bool(env.get("DISPLAY"))
    session = "wayland" if wayland else "x11" if x_display else "headless"
    x11, xtst, libxi, libei = (bool(find(name)) for name in ("X11", "Xtst", "Xi", "ei"))
    return {
        "platform": platform.system(),
        "architecture": platform.machine(),
        "desktop_session": session,
        "x11_runtime_libraries_present": x11 and xtst,
        "xi_runtime_library_present": libxi,
        "libei_present": libei,
        "display_opened": False,
        "input_capture_tested": False,
        "input_injection_tested": False,
        "network_protocol_integrated": False,
        "product_ready": False,
        "note": ("Wayland control is not implemented. Start Hop in an X11 desktop session for keyboard, pointer and clipboard."
                 if wayland else "This inventory checks library presence. Device and desktop compatibility require a connected X11 session."),
    }
