import ctypes.util
import struct
import unittest
import zlib
from hop_linux.clipboard import PNG,validate_payload
from hop_linux.png_decoder import BoundedPNGDecoder
from test_clipboard import png,png_chunk

@unittest.skipUnless(ctypes.util.find_library('gdk_pixbuf-2.0'),'requires GdkPixbuf runtime')
class PNGDecodeTests(unittest.TestCase):
    def test_actual_png_decoder_rejects_structurally_valid_corruption(self):
        decoder=BoundedPNGDecoder();good=png()
        self.assertTrue(decoder(good))
        bad=good[:33]+png_chunk(b'IDAT',b'not zlib')+png_chunk(b'IEND',b'')
        validate_payload(PNG,bad)  # Valid CRC/chunk structure is not enough.
        self.assertFalse(decoder(bad))
        too_much=good[:33]+png_chunk(b'IDAT',zlib.compress(b'\0'*100000))+png_chunk(b'IEND',b'')
        validate_payload(PNG,too_much)
        self.assertFalse(decoder(too_much))

    def test_adam7_interlaced_image(self):
        decoder=BoundedPNGDecoder();w=h=4
        passes=((0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2))
        raw=b''
        for x,y,dx,dy in passes:
            pw=0 if w<=x else (w-x+dx-1)//dx;ph=0 if h<=y else (h-y+dy-1)//dy
            if pw and ph:raw+=(b'\0'+b'\0'*(pw*4))*ph
        data=b'\x89PNG\r\n\x1a\n'+png_chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,1))+png_chunk(b'IDAT',zlib.compress(raw))+png_chunk(b'IEND',b'')
        self.assertTrue(decoder(data))
