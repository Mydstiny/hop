import unittest
from hop_linux.secret_service import SecretStore,SecretServiceError
from hop_linux.tls import TLSLibrary
from test_pairing import cert,fingerprint

class Backend:
    def __init__(self):self.data={}
    def store(self,kind,identity,value):self.data[kind,identity]=value
    def lookup(self,kind,identity):return self.data.get((kind,identity))
    def clear(self,kind,identity):return self.data.pop((kind,identity),None) is not None
    def list_ids(self,kind):return [identity for category,identity in self.data if category==kind]

class SecretStoreTests(unittest.TestCase):
    def test_identity_requires_pair_validator_and_removes(self):
        backend=Backend();tls=object.__new__(TLSLibrary);tls.validate_identity=lambda a,b:(a,b)==(b'cert',b'key')
        store=SecretStore(backend,tls_library=tls)
        with self.assertRaises(ValueError):store.store_identity(b'cert',b'wrong')
        self.assertFalse(backend.data)
        store.store_identity(b'cert',b'key')
        self.assertEqual(store.get_identity(),(b'cert',b'key'))
        tls.validate_identity=lambda *_:False
        store._identity_validator=tls.validate_identity
        with self.assertRaises(SecretServiceError):store.get_identity()
        self.assertTrue(store.remove_identity());self.assertIsNone(store.get_identity())
    def test_no_native_identity_validator_fails_closed(self):
        store=SecretStore(Backend())
        with self.assertRaises(ValueError):store.store_identity(b'cert',b'key')
        with self.assertRaises(ValueError):store.get_identity()
    def test_pin_validation_and_removal(self):
        backend=Backend();store=SecretStore(backend);c=cert(b'peer');pin=fingerprint(c)
        store.store_peer(c,pin);self.assertEqual(store.get_peer(pin).certificate,c)
        backend.data['peer',pin.lower()]=cert(b'other').decode()
        with self.assertRaises(SecretServiceError):store.get_peer(pin)
        self.assertTrue(store.remove_peer(pin));self.assertIsNone(store.get_peer(pin))
        with self.assertRaises(ValueError):store.store_peer(c,'0'*64)

    def test_listing_verifies_each_pin_and_rejects_corrupt_entries(self):
        backend=Backend();store=SecretStore(backend)
        a,b=cert(b'a'),cert(b'b');store.store_peer(a,fingerprint(a));store.store_peer(b,fingerprint(b))
        self.assertEqual(store.list_peers(),tuple(sorted((fingerprint(a).lower(),fingerprint(b).lower()))))
        backend.data['peer',fingerprint(a).lower()]=b.decode('ascii')
        with self.assertRaises(SecretServiceError):store.list_peers()
