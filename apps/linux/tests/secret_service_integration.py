"""Opt-in test against a caller-owned isolated D-Bus Secret Service.

Never run against the user's normal session. The launcher creates its own bus,
keyring and synthetic certificate/key fixture and sets HOP_SECRET_TEST=1.
"""
import os
import sys
if os.environ.get('HOP_SECRET_TEST')!='1':raise SystemExit('isolated Secret Service test required')
from hop_linux.secret_service import SecretServiceError, SecretStore
from test_pairing import cert, fingerprint

if sys.argv[1:] == ['--expect-unavailable']:
    try:SecretStore().get_peer('1'*64)
    except SecretServiceError:print('PASS: missing Secret Service fails without plaintext fallback')
    else:raise AssertionError('unexpected usable service')
elif sys.argv[1:] == ['--expect-available']:
    store=SecretStore();c=cert(b'isolated-public-certificate');pin=fingerprint(c)
    store.store_peer(c,pin);assert store.get_peer(pin).certificate==c
    listed=store.list_peers();assert listed==(pin.lower(),),(listed,pin)
    assert store.remove_peer(pin)
    assert store.get_peer(pin) is None and store.list_peers()==()
    if os.environ.get('HOP_TEST_TLS_LIBRARY') and os.environ.get('HOP_TEST_TLS_FIXTURE_DIR'):
        from hop_linux.tls import TLSLibrary
        from hop_linux import codec
        from hop_linux.pairing import PendingPairing
        from hop_linux.session import Session
        from test_tls import TLSLoopbackTests
        from test_session import Sink
        TLSLoopbackTests.setUpClass()
        library=TLSLibrary(os.environ['HOP_TEST_TLS_LIBRARY'])
        store=SecretStore(tls_library=library)
        assert library.validate_identity(TLSLoopbackTests.certs['a'],TLSLoopbackTests.keys['a'])
        assert not library.validate_identity(TLSLoopbackTests.certs['a'],TLSLoopbackTests.keys['b'])
        store.store_identity(TLSLoopbackTests.certs['a'],TLSLoopbackTests.keys['a'])
        assert store.get_identity()==(TLSLoopbackTests.certs['a'],TLSLoopbackTests.keys['a'])
        assert store.remove_identity() and store.get_identity() is None
        helper=TLSLoopbackTests(methodName='test_bidirectional_bytes_and_timeout')
        client,server=helper.make_pair();host=receiver=None
        try:
            helper.handshake_pair(client,server)
            host=Session(client,codec,Sink(),'host',capabilities=33)
            receiver=Session(server,codec,Sink(),'receiver',capabilities=1)
            host.start();receiver.poll(2);host.poll(2)
            a,b=helper.certs['a'],helper.certs['b']
            left=PendingPairing(a,b,fingerprint);right=PendingPairing(b,a,fingerprint)
            left.confirm(fingerprint(a),fingerprint(b));right.confirm(fingerprint(b),fingerprint(a))
            assert store.get_peer(fingerprint(a)) is None and store.get_peer(fingerprint(b)) is None
            host.confirm_pairing(left);receiver.poll(2);receiver.confirm_pairing(right);host.poll(2)
            left.commit(host,store);right.commit(receiver,store)
            assert store.get_peer(fingerprint(a)).certificate==a and store.get_peer(fingerprint(b)).certificate==b
            assert store.remove_peer(fingerprint(a)) and store.remove_peer(fingerprint(b))
            print('PASS: mutual TLS pairing confirmations persisted into isolated Secret Service')
        finally:
            if host:host.close()
            else:client.close()
            if receiver:receiver.close()
            else:server.close()
    print('PASS: isolated Secret Service identity and peer storage, readback, removal')
else:raise SystemExit('explicit expected test state required')
