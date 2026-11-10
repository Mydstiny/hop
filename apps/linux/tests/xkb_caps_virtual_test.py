"""Probe XKB lock state only on the explicitly created private Xvfb server."""
import ctypes as C
import ctypes.util
import os

class State(C.Structure):
    # XKBstr.h ABI: group bytes, two unsigned shorts, modifier bytes, buttons.
    _fields_=[('group',C.c_ubyte),('locked_group',C.c_ubyte),('base_group',C.c_ushort),('latched_group',C.c_ushort),
              ('mods',C.c_ubyte),('base_mods',C.c_ubyte),('latched_mods',C.c_ubyte),('locked_mods',C.c_ubyte),
              ('compat_state',C.c_ubyte),('grab_mods',C.c_ubyte),('compat_grab_mods',C.c_ubyte),
              ('lookup_mods',C.c_ubyte),('compat_lookup_mods',C.c_ubyte),('ptr_buttons',C.c_ushort)]


def run():
    if os.environ.get('HOP_VIRTUAL_X_SERVER')!='1':raise RuntimeError('virtual X server required')
    from hop_linux.x11 import X11Backend
    lib=C.CDLL(ctypes.util.find_library('X11'))
    lib.XkbGetState.argtypes=[C.c_void_p,C.c_uint,C.POINTER(State)];lib.XkbGetState.restype=C.c_int
    lib.XkbLockModifiers.argtypes=[C.c_void_p,C.c_uint,C.c_uint,C.c_uint];lib.XkbLockModifiers.restype=C.c_int
    def state(display):
        value=State()
        if lib.XkbGetState(display,0x100,C.byref(value))!=0:raise RuntimeError('XKB state unavailable')
        return int(value.locked_mods)
    with X11Backend() as backend:
        display=backend._display;original=state(display)
        try:
            for enabled in (True,False,True):
                if not lib.XkbLockModifiers(display,0x100,2,2 if enabled else 0):raise RuntimeError('XKB lock request failed')
                actual=state(display)
                assert bool(actual&2)==enabled and actual&~2==original&~2
        finally:
            if not lib.XkbLockModifiers(display,0x100,2,original&2):raise RuntimeError('XKB restore request failed')
            assert state(display)==original
    print('PASS: isolated XKB Caps Lock read, set, verify and original-state restoration')
