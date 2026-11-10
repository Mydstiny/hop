import os
import unittest
from hop_linux.clipboard_assembler import ClipboardAssembler,ClipboardAssemblyError

@unittest.skipUnless(os.environ.get('HOP_TEST_CLIPBOARD_LIBRARY'),'requires shared clipboard runtime')
class AssemblerTests(unittest.TestCase):
    def setUp(self):self.a=ClipboardAssembler(os.environ['HOP_TEST_CLIPBOARD_LIBRARY']);self.a.focus(1)
    def tearDown(self):self.a.close()
    def test_chunks_empty_text_and_mutable_output(self):
        self.a.begin(1,1,1,5);self.a.chunk(1,1,0,b'he');self.assertIsNone(self.a.take())
        self.a.chunk(1,1,2,b'llo');kind,epoch,ident,data=self.a.take()
        self.assertEqual((kind,epoch,ident,data),(1,1,1,b'hello'));data[:]=b'\0'*len(data)
        self.a.begin(1,2,1,0);self.assertEqual(self.a.take(),(1,1,2,bytearray()))
    def test_replay_wrong_offset_focus_and_size(self):
        self.a.begin(1,1,1,2);self.a.chunk(1,1,0,b'a')
        with self.assertRaises(ClipboardAssemblyError):self.a.chunk(1,1,0,b'b')
        self.assertIsNone(self.a.take())
        with self.assertRaises(ClipboardAssemblyError):self.a.begin(1,1,1,2)
        self.a.focus(2);self.a.begin(2,2,1,2);self.a.focus(3);self.assertIsNone(self.a.take())
        with self.assertRaises(ClipboardAssemblyError):self.a.begin(3,3,1,2*1024*1024+1)
