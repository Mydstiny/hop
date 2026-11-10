import unittest
from hop_linux.clipboard_x11 import ClipboardX11

class ClipboardTargetTests(unittest.TestCase):
    def test_file_object_targets_are_rejected_but_text_is_not(self):
        for name in ('text/uri-list','x-special/gnome-copied-files','application/x-kde-cutselection','application/vnd.portal.filetransfer','application/x-qt-windows-mime;value="FileGroupDescriptorW"'):
            self.assertTrue(ClipboardX11._file_target(name),name)
        for name in ('UTF8_STRING','text/plain','image/png'):
            self.assertFalse(ClipboardX11._file_target(name),name)
