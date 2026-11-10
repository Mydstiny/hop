"""Cross-platform vectors generated from the shared development specification."""
import json
import os
from pathlib import Path
import unittest
from hop_linux import codec
from hop_linux.session import FrameDecoder, encode_frame

DEFAULT = Path(__file__).resolve().parents[3] / 'protocol/v2-golden-vectors.json'
VECTORS = Path(os.environ.get('HOP_GOLDEN_VECTORS', DEFAULT))

@unittest.skipUnless(VECTORS.is_file(), 'requires shared protocol/v2-golden-vectors.json')
class GoldenTests(unittest.TestCase):
    def test_shared_golden_vectors(self):
        data=json.loads(VECTORS.read_text())
        parsers={1:codec.decode_control,2:codec.decode_input,3:codec.decode_clipboard_control,4:codec.decode_clipboard_chunk}
        for v in data:
            with self.subTest(name=v['name']):
                payload=bytes.fromhex(v['payload_hex']);frame=bytes.fromhex(v['frame_hex'])
                self.assertEqual(encode_frame(v['kind'],v['sequence'],v['epoch'],payload),frame)
                self.assertEqual(FrameDecoder().feed(frame),[(v['kind'],v['sequence'],v['epoch'],payload)])
                if v['valid']: parsers[v['kind']](payload)
                else:
                    with self.assertRaises(codec.CodecError): parsers[v['kind']](payload)
