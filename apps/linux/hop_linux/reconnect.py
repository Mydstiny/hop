"""Explicit pinned reconnection; pre-TLS fingerprint is only a lookup hint."""
import time
from . import codec
from .pair_offer import accept_selected,connect_selected
from .session import Session
from .tls import VerifiedPeer

class ReconnectError(RuntimeError):pass


def reconnect_verified_socket(sock, *, library, own_certificate, private_key, role, tcp_initiator,
                              sink, trust_store, peer_fingerprint=None, capabilities=1, clipboard=None, clipboard_sender=None,
                              timeout=15, prefix=b''):
    """Run on the session owner worker; no first-use fallback or automatic pairing."""
    if role not in ('host','receiver') or type(tcp_initiator) is not bool or not 0<timeout<=30:
        raise ValueError('invalid reconnect request')
    channel=session=None;deadline=time.monotonic()+timeout
    try:
        if tcp_initiator:
            peer=trust_store.get_peer(peer_fingerprint)
            if not isinstance(peer,VerifiedPeer):raise ReconnectError('peer is not trusted')
            channel=connect_selected(sock,library=library,own_certificate=own_certificate,private_key=private_key,peer=peer,timeout=timeout)
        else:
            channel=accept_selected(sock,library=library,own_certificate=own_certificate,private_key=private_key,
                                    lookup_verified_peer=trust_store.get_peer,timeout=timeout,prefix=prefix)
        peer=trust_store.get_peer(channel.peer_fingerprint)
        if not isinstance(peer,VerifiedPeer) or peer.fingerprint!=channel.peer_fingerprint:
            raise ReconnectError('authenticated peer is not trusted')
        if not channel.authenticated or channel.peer_fingerprint!=peer.fingerprint:raise ReconnectError('peer pin mismatch')
        session=Session(channel,codec,sink,role,capabilities=capabilities,clipboard=clipboard,clipboard_sender=clipboard_sender)
        if role=='host':session.start()
        while session.phase=='hello':
            if time.monotonic()>=deadline:raise ReconnectError('reconnect hello timed out')
            session.poll(.05)
        if session.phase=='pairing':raise ReconnectError('peer requires explicit fingerprint confirmation')
        if session.phase!='local':raise ReconnectError('invalid reconnect state')
        return session
    except BaseException:
        if session:session.close()
        elif channel:channel.close()
        else:sock.close()
        raise
