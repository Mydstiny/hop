"""Thread-owned binding to shared bounded clipboard reassembly C ABI.

The caller must authorize focus and capability before passing frames, validate
completed text/PNG off-thread, and erase mutable output after consuming it.
"""
import ctypes as C
import os
import threading

class ClipboardAssemblyError(RuntimeError):pass

class ClipboardAssembler:
    def __init__(self,path):
        if not os.path.isabs(os.fspath(path)):raise ValueError('absolute clipboard runtime path required')
        self.owner=threading.get_ident();self.lib=C.CDLL(os.fspath(path));p=C.c_void_p;s=C.c_size_t;u=C.c_uint64;b=C.c_uint8
        self.lib.hop_clipboard_create.argtypes=[];self.lib.hop_clipboard_create.restype=p
        self.lib.hop_clipboard_focus.argtypes=[p,u];self.lib.hop_clipboard_focus.restype=None
        self.lib.hop_clipboard_begin.argtypes=[p,u,u,b,s];self.lib.hop_clipboard_begin.restype=C.c_int
        self.lib.hop_clipboard_chunk.argtypes=[p,u,u,s,C.POINTER(b),s];self.lib.hop_clipboard_chunk.restype=C.c_int
        self.lib.hop_clipboard_info.argtypes=[p,C.POINTER(b),C.POINTER(u),C.POINTER(u),C.POINTER(s)];self.lib.hop_clipboard_info.restype=C.c_int
        self.lib.hop_clipboard_take.argtypes=[p,C.POINTER(b),s];self.lib.hop_clipboard_take.restype=C.c_int
        self.lib.hop_clipboard_cancel.argtypes=[p];self.lib.hop_clipboard_cancel.restype=None
        self.lib.hop_clipboard_free.argtypes=[p];self.lib.hop_clipboard_free.restype=None
        self.handle=self.lib.hop_clipboard_create()
        if not self.handle:raise ClipboardAssemblyError('cannot initialize clipboard assembler')
    def _check(self):
        if threading.get_ident()!=self.owner:raise RuntimeError('clipboard assembler is thread-owned')
        if not self.handle:raise ClipboardAssemblyError('clipboard assembler closed')
    def focus(self,epoch):
        self._check()
        if type(epoch) is not int or not 0<=epoch<2**64:raise ValueError('invalid focus epoch')
        self.lib.hop_clipboard_focus(self.handle,epoch)
    def begin(self,epoch,transfer_id,format,total):
        self._check()
        if (any(type(v) is not int for v in (epoch,transfer_id,format,total)) or not 1<=epoch<2**64 or
            not 1<=transfer_id<2**64 or format not in (1,2) or total<0 or total>(2*1024*1024 if format==1 else 16*1024*1024)):
            self.cancel();raise ClipboardAssemblyError('invalid clipboard start')
        if self.lib.hop_clipboard_begin(self.handle,epoch,transfer_id,format,total)!=0:
            self.cancel();raise ClipboardAssemblyError('clipboard start rejected')
    def chunk(self,epoch,transfer_id,offset,data):
        self._check()
        if (any(type(v) is not int for v in (epoch,transfer_id,offset)) or not 1<=epoch<2**64 or
            not 1<=transfer_id<2**64 or not 0<=offset<=16*1024*1024 or not isinstance(data,bytes) or not 1<=len(data)<=65520):
            self.cancel();raise ClipboardAssemblyError('invalid clipboard chunk')
        buf=(C.c_uint8*len(data)).from_buffer_copy(data)
        try:
            if self.lib.hop_clipboard_chunk(self.handle,epoch,transfer_id,offset,buf,len(data))!=0:
                self.cancel();raise ClipboardAssemblyError('clipboard chunk rejected')
        finally:C.memset(buf,0,len(data))
    def take(self):
        self._check();fmt=C.c_uint8();epoch=C.c_uint64();ident=C.c_uint64();size=C.c_size_t()
        result=self.lib.hop_clipboard_info(self.handle,C.byref(fmt),C.byref(epoch),C.byref(ident),C.byref(size))
        if result==0:return None
        if result!=1 or fmt.value not in (1,2) or size.value>(2*1024*1024 if fmt.value==1 else 16*1024*1024):
            self.cancel();raise ClipboardAssemblyError('invalid completed clipboard value')
        output=bytearray(size.value);buf=(C.c_uint8*size.value).from_buffer(output) if size.value else None
        if self.lib.hop_clipboard_take(self.handle,buf,size.value)!=0:
            if output:output[:]=b'\0'*len(output)
            self.cancel();raise ClipboardAssemblyError('clipboard take failed')
        return fmt.value,epoch.value,ident.value,output
    def cancel(self):
        self._check();self.lib.hop_clipboard_cancel(self.handle)
    def close(self):
        if threading.get_ident()!=self.owner:raise RuntimeError('clipboard assembler is thread-owned')
        if self.handle:self.lib.hop_clipboard_free(self.handle);self.handle=None
