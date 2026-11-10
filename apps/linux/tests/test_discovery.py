import unittest
from hop_linux.discovery import DiscoveryError,validate_hint,SERVICE

class DiscoveryTests(unittest.TestCase):
    def test_untrusted_hint_validation(self):
        hint=validate_hint('设备','192.168.1.2',4321,1,[b'v=2'])
        self.assertEqual((hint.address,hint.port),('192.168.1.2',4321));self.assertEqual(SERVICE,'_hop2._tcp')
        for address in ('0.0.0.0','224.0.0.1','127.0.0.1','evil.example'):
            with self.assertRaises(DiscoveryError):validate_hint('x',address,1,1,[b'v=2'])
        for txt in ([],[b'v=1'],[b'v=2',b'cert=unknown']):
            with self.assertRaises(DiscoveryError):validate_hint('x','192.168.1.2',1,1,txt)
