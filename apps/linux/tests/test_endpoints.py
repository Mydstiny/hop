import tempfile
import unittest
from pathlib import Path
from hop_linux.endpoints import EndpointStore


class EndpointStoreTests(unittest.TestCase):
    def test_roundtrip_and_invalid_addresses(self):
        with tempfile.TemporaryDirectory() as root:
            store=EndpointStore(Path(root)/'endpoints.json');pin='a'*64
            store.update(pin,'Mac','192.168.1.2',24891)
            self.assertEqual(store.get(pin),{'name':'Mac','address':'192.168.1.2','port':24891})
            for address in ('0.0.0.0','239.1.1.1','example.com','fe80::1'):
                with self.assertRaises(ValueError):store.update(pin,'name',address,1)
            self.assertEqual(store.get(pin)['address'],'192.168.1.2')

    def test_corrupt_file_and_symlink_are_preserved(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'endpoints.json';path.write_text('{invalid')
            store=EndpointStore(path)
            with self.assertRaises(ValueError):store.update('a'*64,'name','127.0.0.1',1)
            self.assertEqual(path.read_text(),'{invalid')
            other=Path(root)/'other';other.write_text('untouched');link=Path(root)/'link';link.symlink_to(other)
            with self.assertRaises(ValueError):EndpointStore(link).update('a'*64,'name','127.0.0.1',1)
            self.assertEqual(other.read_text(),'untouched')
