import unittest
from hop_linux import codec
from hop_linux.clipboard import TEXT
from hop_linux.clipboard_session import ClipboardSender

class ClipboardSenderTests(unittest.TestCase):
    def test_one_chunk_at_a_time_and_version_cancel(self):
        sender=ClipboardSender();sender.focus(7)
        with self.assertRaises(PermissionError):sender.start(6,TEXT,b'x',1)
        ident=sender.start(7,TEXT,b'x'*70000,1);kind,start=sender.next_frame(7,1)
        self.assertEqual((kind,codec.decode_clipboard_control(start)[1]['transfer_id']),(3,ident))
        kind,chunk=sender.next_frame(7,1);self.assertEqual(kind,4);self.assertEqual(len(codec.decode_clipboard_chunk(chunk)[1]['data']),65520)
        kind,cancel=sender.next_frame(7,2);self.assertEqual((kind,codec.decode_clipboard_control(cancel)[0]),(3,'cancel'))
        self.assertIsNone(sender.next_frame(7,2));sender.close()
    def test_empty_text_and_focus_change(self):
        sender=ClipboardSender();sender.focus(1);sender.start(1,TEXT,b'',1)
        self.assertEqual(codec.decode_clipboard_control(sender.next_frame(1,1)[1])[1]['total'],0)
        self.assertIsNone(sender.next_frame(1,1));sender.start(1,TEXT,b'a',1);sender.focus(2)
        self.assertIsNone(sender.next_frame(1,1));sender.close()
