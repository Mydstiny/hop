"""Private subprocess: bounded PNG decode without accessing a display."""
import ctypes as C
import ctypes.util
import resource
import struct
import sys
import zlib


def _validate_compressed_rows(data):
    """Bound zlib output exactly to the IHDR/Adam7 scanline geometry."""
    width,height,depth,color,compression,filter_method,interlace=struct.unpack_from('>IIBBBBB',data,16)
    depths={0:{1,2,4,8,16},2:{8,16},3:{1,2,4,8},4:{8,16},6:{8,16}}
    channels={0:1,2:3,3:1,4:2,6:4}
    if color not in depths or depth not in depths[color] or compression or filter_method or interlace not in (0,1):
        raise ValueError('unsupported PNG header')
    passes=((0,0,1,1),) if not interlace else ((0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2))
    rows=[]
    for x0,y0,dx,dy in passes:
        w=0 if width<=x0 else (width-x0+dx-1)//dx
        h=0 if height<=y0 else (height-y0+dy-1)//dy
        if w and h:rows.extend([1+(w*channels[color]*depth+7)//8]*h)
    expected=sum(rows)
    if not rows or expected>160*1024*1024:raise ValueError('PNG output exceeds bound')
    offsets=[];offset=8;seen_data=False;ended_data=False
    while offset+12<=len(data):
        size=struct.unpack_from('>I',data,offset)[0];kind=data[offset+4:offset+8]
        if kind==b'IDAT':
            if ended_data:raise ValueError('non-consecutive PNG data')
            seen_data=True;offsets.append((offset+8,size))
        elif seen_data:ended_data=True
        offset+=size+12
        if kind==b'IEND':break
    stream=zlib.decompressobj();produced=0;row=0;remaining=0
    for start,size in offsets:
        chunk=data[start:start+size]
        if stream.eof and chunk:raise ValueError('extra compressed PNG data')
        while chunk:
            output=stream.decompress(chunk,min(65536,expected-produced+1));chunk=stream.unconsumed_tail
            if stream.unused_data:raise ValueError('extra compressed PNG data')
            produced+=len(output)
            if produced>expected:raise ValueError('PNG output exceeds geometry')
            position=0
            while position<len(output):
                if not remaining:
                    if row>=len(rows) or output[position]>4:raise ValueError('invalid PNG scanline filter')
                    remaining=rows[row];row+=1
                count=min(remaining,len(output)-position);position+=count;remaining-=count
            if not chunk:break
    if not stream.eof or produced!=expected or row!=len(rows) or remaining:
        raise ValueError('incomplete PNG compressed data')


def main():
    # The process can be killed independently of the GUI/input controller.
    ceiling=512*1024*1024
    resource.setrlimit(resource.RLIMIT_AS,(ceiling,ceiling));resource.setrlimit(resource.RLIMIT_CPU,(3,3))
    from .clipboard import MAX_PNG,PNG,validate_payload
    image=ctypes.util.find_library('gdk_pixbuf-2.0');objects=ctypes.util.find_library('gobject-2.0');glib=ctypes.util.find_library('glib-2.0')
    if not image or not objects or not glib:raise RuntimeError('PNG decoding runtime unavailable')
    lib,objectlib,gliblib=C.CDLL(image),C.CDLL(objects),C.CDLL(glib)
    p=C.c_void_p;z=C.c_size_t;i=C.c_int
    lib.gdk_pixbuf_loader_new_with_type.argtypes=[C.c_char_p,C.POINTER(p)];lib.gdk_pixbuf_loader_new_with_type.restype=p
    lib.gdk_pixbuf_loader_write.argtypes=[p,C.POINTER(C.c_ubyte),z,C.POINTER(p)];lib.gdk_pixbuf_loader_write.restype=i
    lib.gdk_pixbuf_loader_close.argtypes=[p,C.POINTER(p)];lib.gdk_pixbuf_loader_close.restype=i
    lib.gdk_pixbuf_loader_get_pixbuf.argtypes=[p];lib.gdk_pixbuf_loader_get_pixbuf.restype=p
    for name in ('gdk_pixbuf_get_width','gdk_pixbuf_get_height','gdk_pixbuf_get_rowstride'):
        f=getattr(lib,name);f.argtypes=[p];f.restype=i
    objectlib.g_object_unref.argtypes=[p];gliblib.g_error_free.argtypes=[p]
    err=p();loader=lib.gdk_pixbuf_loader_new_with_type(b'png',C.byref(err))
    if not loader:
        if err:gliblib.g_error_free(err)
        raise RuntimeError('PNG loader unavailable')
    try:
        if sys.argv[1:]==['--probe']:return
        if sys.argv[1:]:raise ValueError('invalid operation')
        data=sys.stdin.buffer.read(MAX_PNG+1)
        validate_payload(PNG,data)
        _validate_compressed_rows(data)
        # PNG dimensions are checked structurally before the decoder allocates.
        for start in range(0,len(data),65536):
            part=data[start:start+65536];buf=(C.c_ubyte*len(part)).from_buffer_copy(part)
            err=p()
            if not lib.gdk_pixbuf_loader_write(loader,buf,len(part),C.byref(err)):
                if err:gliblib.g_error_free(err)
                raise ValueError('PNG decoding failed')
        err=p()
        if not lib.gdk_pixbuf_loader_close(loader,C.byref(err)):
            if err:gliblib.g_error_free(err)
            raise ValueError('PNG decoding failed')
        pixbuf=lib.gdk_pixbuf_loader_get_pixbuf(loader)
        if not pixbuf:raise ValueError('PNG has no pixels')
        w,h,stride=(getattr(lib,n)(pixbuf) for n in ('gdk_pixbuf_get_width','gdk_pixbuf_get_height','gdk_pixbuf_get_rowstride'))
        expected=struct.unpack_from('>II',data,16)
        if (w,h)!=expected or w<=0 or h<=0 or w*h>4096*4096 or stride<=0 or stride*h>128*1024*1024:raise ValueError('PNG decoded size exceeds limit')
    finally:objectlib.g_object_unref(loader)

if __name__=='__main__':
    try:main()
    except BaseException:sys.exit(1)
