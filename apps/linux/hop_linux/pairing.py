"""User-confirmed certificate comparison; no discovery or trust-on-first-use."""
import hmac
import re
import time
from .tls import VerifiedPeer


class PairingError(RuntimeError): pass


class PendingPairing:
    """Single-use pending comparison. Storage is an explicitly supplied backend.

    Both displayed fingerprints must be independently compared through a
    trusted channel; supplying the displayed strings is only the caller's
    explicit confirmation, not automatic proof of user presence.
    """
    def __init__(self, own_certificate, peer_certificate, fingerprint_calculator, *, now=time.monotonic, timeout=120):
        if not callable(fingerprint_calculator) or not callable(now) or not 1<=timeout<=600:
            raise ValueError('invalid pairing parameters')
        self._now=now;self._deadline=now()+timeout;self._closed=False
        self.locally_verified=False
        self.local_fingerprint=self._fingerprint(fingerprint_calculator(own_certificate))
        self.peer_fingerprint=self._fingerprint(fingerprint_calculator(peer_certificate))
        # Independently compare with DER-based SHA-256 and validate the PEM shape.
        self._peer=VerifiedPeer(peer_certificate,self.peer_fingerprint)
        if hmac.compare_digest(self.local_fingerprint,self.peer_fingerprint): raise ValueError('peer must have a distinct identity')

    @staticmethod
    def _fingerprint(value):
        if not isinstance(value,str) or not re.fullmatch(r'[0-9a-fA-F]{64}',value): raise ValueError('invalid fingerprint')
        return value.upper()

    def cancel(self): self._closed=True;self._peer=None

    def confirm(self, local_fingerprint, peer_fingerprint):
        if self._closed: raise PairingError('pairing no longer pending')
        try:
            if self._now()>=self._deadline: raise PairingError('pairing expired')
            local=self._fingerprint(local_fingerprint);remote=self._fingerprint(peer_fingerprint)
            if not hmac.compare_digest(local,self.local_fingerprint) or not hmac.compare_digest(remote,self.peer_fingerprint):
                raise PairingError('fingerprints do not match')
            self.locally_verified=True
            return self._peer
        except BaseException:
            self.cancel();raise

    def verify_session(self, transport):
        if (self._closed or not self.locally_verified or self._now()>=self._deadline or
            not transport.authenticated or transport.local_fingerprint != self.local_fingerprint.lower() or
            transport.peer_fingerprint != self._peer.fingerprint):
            raise PairingError('verified candidate does not match authenticated session')

    def verified_peer(self):
        if self._closed or not self.locally_verified or self._now()>=self._deadline:
            raise PairingError('candidate has not been verified or expired')
        return self._peer

    def commit(self, session, store):
        try:
            self.verify_session(session.transport)
            if not session.pairing_confirmed: raise PairingError('both TLS confirmations are required')
            if not callable(getattr(store,'store_peer',None)): raise ValueError('explicit trust store required')
            peer=self._peer
            store.store_peer(peer.certificate,peer.fingerprint)
            return peer
        finally:self.cancel()
