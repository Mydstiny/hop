"""Explicit worker-thread flow from verified candidate to pinned v2 session.

Discovery/GUI only supply hints and explicit user confirmation. This blocking
workflow does not run on the GTK or X11 capture thread. The caller must keep
polling the returned session on the same worker thread.
"""
import time
from . import codec
from .pair_offer import accept_selected,connect_selected
from .pairing import PendingPairing,PairingError
from .session import Session


def pair_verified_socket(sock, *, pending, library, own_certificate, private_key,
                         role, tcp_initiator, sink, trust_store, capabilities=1,
                         clipboard=None, clipboard_sender=None, timeout=15, canceled=lambda:False):
    if (not isinstance(pending,PendingPairing) or role not in ('host','receiver') or
        type(tcp_initiator) is not bool or not 0<timeout<=30):raise ValueError('invalid verified pairing request')
    peer=pending.verified_peer();session=None;channel=None;deadline=time.monotonic()+timeout
    try:
        if tcp_initiator:
            channel=connect_selected(sock,library=library,own_certificate=own_certificate,private_key=private_key,peer=peer,timeout=timeout)
        else:
            channel=accept_selected(sock,library=library,own_certificate=own_certificate,private_key=private_key,
                                    lookup_verified_peer=lambda hint:peer if hint.lower()==peer.fingerprint else None,timeout=timeout)
        pending.verify_session(channel)
        session=Session(channel,codec,sink,role,capabilities=capabilities|32,clipboard=clipboard,clipboard_sender=clipboard_sender)
        if role=='host':session.start()
        while session.phase=='hello':
            if canceled():raise PairingError('pairing canceled')
            if time.monotonic()>=deadline:raise PairingError('pairing hello timed out')
            session.poll(.05)
        if session.phase!='pairing':raise PairingError('pairing confirmation was not negotiated')
        session.confirm_pairing(pending)
        while not session.pairing_confirmed:
            if canceled():raise PairingError('pairing canceled')
            if time.monotonic()>=deadline:raise PairingError('peer pairing confirmation timed out')
            session.poll(.05)
        if canceled():raise PairingError('pairing canceled')
        pending.commit(session,trust_store)
        return session
    except BaseException:
        pending.cancel()
        if session:session.close()
        elif channel:channel.close()
        else:sock.close()
        raise
