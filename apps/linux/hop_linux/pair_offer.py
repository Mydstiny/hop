"""Bounded, explicitly initiated v2 candidate exchange and pin lookup.

Candidate certificates and names are untrusted; this code never persists them
or performs first-use trust. Candidate sockets close before return; a separate
connection with certificate-pinned TLS is required after user verification.
"""
from dataclasses import dataclass
import re
import socket
import struct
import time
import threading
import unicodedata

_HEADER=struct.Struct('!4sBBHI16s')
_MAX_NAME=128;_MAX_CERT=16384;_MAX_FRAME=_HEADER.size+_MAX_NAME+_MAX_CERT

class OfferError(RuntimeError): pass

@dataclass(frozen=True)
class Candidate:
    role:int
    name:str
    certificate:bytes
    nonce:bytes


def _name(name):
    if not isinstance(name,str):raise OfferError('invalid candidate name')
    try:data=name.encode('utf8')
    except UnicodeEncodeError as exc:raise OfferError('invalid candidate name') from exc
    if not 1<=len(data)<=_MAX_NAME:raise OfferError('invalid candidate name length')
    if any(unicodedata.category(c)=='Cc' or ord(c) in (0x061c,0x200e,0x200f) or 0x2028<=ord(c)<=0x202e or 0x2066<=ord(c)<=0x2069 for c in name):
        raise OfferError('invalid candidate name characters')
    return data


def encode_offer(candidate, certificate_fingerprint):
    if not isinstance(candidate,Candidate) or candidate.role not in (1,2) or not isinstance(candidate.nonce,bytes) or len(candidate.nonce)!=16 or not any(candidate.nonce):raise OfferError('invalid candidate')
    name=_name(candidate.name);certificate=candidate.certificate
    if not isinstance(certificate,bytes) or not 1<=len(certificate)<=_MAX_CERT:raise OfferError('invalid certificate size')
    try:certificate_fingerprint(certificate)
    except Exception as exc:raise OfferError('invalid candidate certificate') from exc
    return _HEADER.pack(b'HPOP',1,candidate.role,len(name),len(certificate),candidate.nonce)+name+certificate


def decode_offer(data, certificate_fingerprint):
    if not isinstance(data,bytes) or not _HEADER.size<=len(data)<=_MAX_FRAME:raise OfferError('invalid offer size')
    magic,version,role,nlen,clen,nonce=_HEADER.unpack_from(data)
    if magic!=b'HPOP' or version!=1 or role not in (1,2) or not 1<=nlen<=_MAX_NAME or not 1<=clen<=_MAX_CERT or len(data)!=_HEADER.size+nlen+clen or not any(nonce):raise OfferError('invalid offer header')
    try:name=data[_HEADER.size:_HEADER.size+nlen].decode('utf8')
    except UnicodeDecodeError as exc:raise OfferError('invalid UTF-8 name') from exc
    candidate=Candidate(role,name,data[_HEADER.size+nlen:],nonce)
    encode_offer(candidate,certificate_fingerprint)
    return candidate


def exchange_candidate(sock, *, own_role, name, certificate, tls_library, explicitly_started, tcp_initiator=None, timeout=10, prefix=b''):
    if not isinstance(sock,socket.socket) or own_role not in (1,2) or explicitly_started is not True or not 0<timeout<=30:raise OfferError('explicit bounded candidate exchange required')
    if tcp_initiator is None:tcp_initiator=own_role==1
    if type(tcp_initiator) is not bool or prefix not in (b'',b'HPOP') or (prefix and tcp_initiator):raise OfferError('invalid candidate direction or prefix')
    try:
        deadline=time.monotonic()+timeout
        nonce=tls_library.random_bytes(16)
        offer=encode_offer(Candidate(own_role,name,certificate,nonce),tls_library.certificate_fingerprint)
        def send_offer():
            remaining=deadline-time.monotonic()
            if remaining<=0:raise OfferError('candidate exchange timed out')
            sock.settimeout(remaining);sock.sendall(offer);sock.shutdown(socket.SHUT_WR)
        def receive_offer():
            data=bytearray(prefix)
            while True:
                remaining=deadline-time.monotonic()
                if remaining<=0:raise OfferError('candidate exchange timed out')
                sock.settimeout(remaining);block=sock.recv(min(4096,_MAX_FRAME+1-len(data)))
                if not block:break
                data.extend(block)
                if len(data)>_MAX_FRAME:raise OfferError('candidate frame too large')
            candidate=decode_offer(bytes(data),tls_library.certificate_fingerprint)
            if candidate.role==own_role:raise OfferError('candidate role conflict')
            return candidate
        if tcp_initiator:
            send_offer();candidate=receive_offer()
        else:
            candidate=receive_offer();send_offer()
        return candidate
    except (OSError,ValueError) as exc:raise OfferError('candidate exchange failed') from exc
    finally:sock.close()


def encode_selector(fingerprint):
    if not isinstance(fingerprint,str) or not re.fullmatch('[0-9A-F]{64}',fingerprint):raise OfferError('invalid lookup hint')
    return b'HPSL'+fingerprint.encode('ascii')


def decode_selector(data):
    if not isinstance(data,bytes) or len(data)!=68 or data[:4]!=b'HPSL':raise OfferError('invalid lookup hint')
    try:value=data[4:].decode('ascii')
    except UnicodeDecodeError as exc:raise OfferError('invalid lookup hint') from exc
    if encode_selector(value)!=data:raise OfferError('invalid lookup hint')
    return value


def read_connection_prefix(sock, *, timeout=5):
    """Read exactly four untrusted preface bytes without consuming TLS data."""
    if not isinstance(sock,socket.socket) or not 0<timeout<=30:raise OfferError('invalid prefix read')
    data=bytearray();deadline=time.monotonic()+timeout
    try:
        while len(data)<4:
            remaining=deadline-time.monotonic()
            if remaining<=0:raise OfferError('preface timed out')
            sock.settimeout(remaining);part=sock.recv(4-len(data))
            if not part:raise OfferError('truncated preface')
            data.extend(part)
        if bytes(data) not in (b'HPOP',b'HPSL'):raise OfferError('unknown preface')
        return bytes(data)
    except OSError as exc:raise OfferError('preface read failed') from exc


def read_selector(sock, *, timeout=10, prefix=b''):
    """Read exactly 68 bytes; leave any already queued TLS bytes untouched."""
    if not isinstance(sock,socket.socket) or not 0<timeout<=30:raise OfferError('invalid selector read')
    if prefix not in (b'',b'HPSL'):raise OfferError('invalid selector prefix')
    deadline=time.monotonic()+timeout;data=bytearray(prefix)
    try:
        while len(data)<68:
            remaining=deadline-time.monotonic()
            if remaining<=0:raise OfferError('lookup hint timed out')
            sock.settimeout(remaining)
            part=sock.recv(68-len(data))
            if not part:raise OfferError('truncated lookup hint')
            data.extend(part)
        return decode_selector(bytes(data))
    except OSError as exc:raise OfferError('lookup hint read failed') from exc


def connect_selected(sock, *, library, own_certificate, private_key, peer, timeout=10):
    """Write our public lookup hint then authenticate the exact pinned peer."""
    from .tls import SecureSocket, VerifiedPeer
    if not isinstance(sock,socket.socket) or not isinstance(peer,VerifiedPeer) or not 0<timeout<=30:raise OfferError('verified peer and bounded timeout required')
    channel=None;deadline=time.monotonic()+timeout
    try:
        hint=encode_selector(library.certificate_fingerprint(own_certificate))
        remaining=deadline-time.monotonic()
        if remaining<=0:raise OfferError('connection timed out')
        sock.settimeout(remaining);sock.sendall(hint)
        channel=SecureSocket(library,sock,client=True,own_certificate=own_certificate,private_key=private_key,peer=peer)
        remaining=deadline-time.monotonic()
        if remaining<=0:raise OfferError('connection timed out')
        channel.handshake(timeout=remaining)
        return channel
    except BaseException:
        if channel:channel.close()
        else:sock.close()
        raise


def accept_selected(sock, *, library, own_certificate, private_key, lookup_verified_peer, timeout=10, prefix=b''):
    """Lookup an already verified exact certificate before authenticating TLS.

    lookup_verified_peer must use a stored pin or an explicitly user-verified
    short-lived candidate; an unknown hint must return None.
    """
    from .tls import SecureSocket, VerifiedPeer
    if not isinstance(sock,socket.socket) or not callable(lookup_verified_peer) or not 0<timeout<=30:raise OfferError('verified lookup and bounded timeout required')
    channel=None;deadline=time.monotonic()+timeout
    try:
        hint=read_selector(sock,timeout=timeout,prefix=prefix);peer=lookup_verified_peer(hint)
        if not isinstance(peer,VerifiedPeer) or peer.fingerprint != hint.lower():raise OfferError('unknown verified peer')
        channel=SecureSocket(library,sock,client=False,own_certificate=own_certificate,private_key=private_key,peer=peer)
        remaining=deadline-time.monotonic()
        if remaining<=0:raise OfferError('connection timed out')
        channel.handshake(timeout=remaining)
        return channel
    except BaseException:
        if channel:channel.close()
        else:sock.close()
        raise


class CandidateWindow:
    """A short-lived, caller-owned listener for one explicitly initiated pairing.

    Accepts serially, caps failed attempts and applies a minimum interval.
    Applications must run this blocking interface on a worker, display the
    resulting *untrusted* candidate and close/cancel the window with the UI.
    """
    def __init__(self, listener, *, own_role, name, certificate, tls_library,
                 explicitly_started, lifetime=60, max_attempts=8, minimum_interval=.25, keep_listener=False):
        if (not isinstance(listener,socket.socket) or explicitly_started is not True or
            listener.getsockopt(socket.SOL_SOCKET,socket.SO_ACCEPTCONN)!=1 or own_role not in (1,2) or
            not 1<=lifetime<=120 or type(max_attempts) is not int or not 1<=max_attempts<=8 or
            not .25<=minimum_interval<=10 or type(keep_listener) is not bool): raise OfferError('invalid candidate window')
        _name(name);tls_library.certificate_fingerprint(certificate)
        self.listener,self.role,self.name,self.certificate,self.library=listener,own_role,name,certificate,tls_library
        self.deadline=time.monotonic()+lifetime;self.max_attempts=max_attempts;self.minimum_interval=minimum_interval
        self.attempts=0;self.last_attempt=None;self.closed=False;self.lock=threading.Lock();self.active=None
        self.keep_listener=keep_listener

    def accept_once(self):
        if not self.lock.acquire(blocking=False):raise OfferError('candidate exchange already running')
        try:
            now=time.monotonic()
            if self.closed or now>=self.deadline or self.attempts>=self.max_attempts:
                self.close();raise OfferError('candidate window expired')
            if self.last_attempt is not None and now-self.last_attempt<self.minimum_interval:
                raise OfferError('candidate attempts rate limited')
            self.listener.settimeout(self.deadline-now)
            try:connection,_=self.listener.accept()
            except OSError as exc:raise OfferError('candidate accept failed or timed out') from exc
            if self.closed:connection.close();raise OfferError('candidate window closed')
            self.active=connection
            self.attempts+=1;self.last_attempt=time.monotonic()
            remaining=self.deadline-self.last_attempt
            if remaining<=0:
                connection.close();self.close();raise OfferError('candidate window expired')
            candidate=exchange_candidate(connection,own_role=self.role,name=self.name,certificate=self.certificate,tcp_initiator=False,
                                         tls_library=self.library,explicitly_started=True,timeout=min(10,remaining))
            if self.keep_listener:
                self.active=None;self.closed=True
            else:self.close()
            return candidate
        finally:self.lock.release()

    def close(self):
        if not self.closed:
            self.closed=True
            if self.active:
                try:self.active.shutdown(socket.SHUT_RDWR)
                except OSError:pass
                self.active.close()
            try:self.listener.shutdown(socket.SHUT_RDWR)
            except OSError:pass
            self.listener.close()
