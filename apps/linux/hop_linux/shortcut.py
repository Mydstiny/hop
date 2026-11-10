"""Serialized binding to the shared stateful shortcut C ABI."""
import ctypes as C
import os
import threading

_MAX_EVENTS=240

class ShortcutError(RuntimeError): pass

class _Key(C.Structure):
    _fields_=[('usage',C.c_uint16),('modifiers',C.c_uint16),('down',C.c_uint8),('repeat',C.c_uint8)]

class ShortcutTranslator:
    def __init__(self, library_path, source, target, *, mode=1):
        if not os.path.isabs(os.fspath(library_path)): raise ValueError('absolute runtime path required')
        if source not in (1,2,3,4) or target not in (1,2,3,4) or mode not in (0,1): raise ValueError('invalid shortcut mode or platform')
        self._owner=threading.get_ident();self._failed=False;self._lib=C.CDLL(os.fspath(library_path))
        lib=self._lib;ptr=C.c_void_p;events=C.POINTER(_Key);size=C.c_size_t
        lib.hop_shortcut_create.argtypes=[C.c_int,C.c_int,C.c_int];lib.hop_shortcut_create.restype=ptr
        lib.hop_shortcut_push.argtypes=[ptr,events,events,size,C.POINTER(size)];lib.hop_shortcut_push.restype=C.c_int
        for name in ('hop_shortcut_flush_pointer','hop_shortcut_release'):
            f=getattr(lib,name);f.argtypes=[ptr,events,size,C.POINTER(size)];f.restype=C.c_int
        lib.hop_shortcut_free.argtypes=[ptr];lib.hop_shortcut_free.restype=None
        self._handle=lib.hop_shortcut_create(source,target,mode)
        if not self._handle: raise ShortcutError('shortcut translator creation failed')

    def _check(self, *, allow_failed=False):
        if threading.get_ident()!=self._owner: raise RuntimeError('shortcut translator is thread-owned')
        if not self._handle or (self._failed and not allow_failed): raise ShortcutError('shortcut translator unavailable')

    def _call(self,name,event=None):
        self._check(allow_failed=name=='hop_shortcut_release')
        out=(_Key*_MAX_EVENTS)();count=C.c_size_t()
        f=getattr(self._lib,name)
        if event is None: status=f(self._handle,out,_MAX_EVENTS,C.byref(count))
        else: status=f(self._handle,C.byref(event),out,_MAX_EVENTS,C.byref(count))
        if status!=0 or count.value>_MAX_EVENTS:
            self._failed=True;raise ShortcutError('shortcut translation failed')
        return [dict(usage=v.usage,modifiers=v.modifiers,down=v.down,repeat=v.repeat) for v in out[:count.value]]

    def push(self, *, usage, modifiers, down, repeat):
        if (any(type(x) is not int for x in (usage,modifiers,down,repeat)) or not 1<=usage<=65535 or
            not 0<=modifiers<=511 or down not in (0,1) or repeat not in (0,1)):
            self._failed=True;raise ShortcutError('invalid shortcut input')
        return self._call('hop_shortcut_push',_Key(usage,modifiers,down,repeat))

    def flush_pointer(self):return self._call('hop_shortcut_flush_pointer')
    def release(self):return self._call('hop_shortcut_release')
    def close(self):
        if threading.get_ident()!=self._owner: raise RuntimeError('shortcut translator is thread-owned')
        if self._handle:self._lib.hop_shortcut_free(self._handle);self._handle=None
