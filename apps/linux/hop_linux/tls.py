"""Explicitly pinned, nonblocking socket transport via Hop's TLS C ABI.

The caller must independently verify the peer fingerprint before constructing
VerifiedPeer. There is no discovery, first-use trust, pairing, protocol framing,
or plaintext fallback in this module. Private key lifecycle/storage belongs to
the caller; Python does not guarantee erasure of immutable byte strings.
"""
from __future__ import annotations

import ctypes as C
from dataclasses import dataclass
import hashlib
import hmac
import os
import re
import select
import socket
import ssl
import threading
import time

_MAX_PEM = 65536
_MAX_IO = 1024 * 1024
_SEND = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(C.c_ubyte), C.c_size_t)
_RECV = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(C.c_ubyte), C.c_size_t)


class TLSError(RuntimeError):
    pass


def _pem_bytes(value, kind):
    if not isinstance(value, bytes) or not value or len(value) > _MAX_PEM or b"\0" in value:
        raise ValueError("invalid " + kind + " PEM")
    return value


@dataclass(frozen=True, init=False)
class VerifiedPeer:
    """Certificate pinned to a separately verified SHA-256 DER fingerprint."""
    certificate: bytes
    fingerprint: str
    def __init__(self, certificate, verified_fingerprint):
        cert = _pem_bytes(certificate, "peer certificate")
        if not isinstance(verified_fingerprint, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", verified_fingerprint):
            raise ValueError("verified peer fingerprint must be 64 hexadecimal characters")
        if cert.count(b"-----BEGIN CERTIFICATE-----") != 1 or cert.count(b"-----END CERTIFICATE-----") != 1:
            raise ValueError("exactly one peer certificate is required")
        try:
            pem = cert.decode("ascii")
            if not re.fullmatch(r"\s*-----BEGIN CERTIFICATE-----\s+[A-Za-z0-9+/=\s]+-----END CERTIFICATE-----\s*", pem):
                raise ValueError("invalid certificate envelope")
            der = ssl.PEM_cert_to_DER_cert(pem.strip())
        except (UnicodeDecodeError, ValueError) as exc:
            raise ValueError("invalid peer certificate") from exc
        actual = hashlib.sha256(der).hexdigest()
        if not hmac.compare_digest(actual, verified_fingerprint.lower()):
            raise ValueError("verified fingerprint does not match peer certificate")
        object.__setattr__(self, "certificate", cert)
        object.__setattr__(self, "fingerprint", actual)


class TLSLibrary:
    """Bind a known, explicitly supplied shared library path."""
    def __init__(self, path):
        if not os.path.isabs(os.fspath(path)):
            raise ValueError("TLS library path must be absolute")
        self.lib = C.CDLL(os.fspath(path))
        lib = self.lib
        lib.hop_tls_want_read_error.restype = C.c_int
        lib.hop_tls_want_write_error.restype = C.c_int
        lib.hop_tls_create.argtypes = [C.c_int, C.c_char_p, C.c_char_p, C.c_char_p, C.c_void_p, _SEND, _RECV]
        lib.hop_tls_create.restype = C.c_void_p
        lib.hop_tls_handshake.argtypes = [C.c_void_p]; lib.hop_tls_handshake.restype = C.c_int
        lib.hop_tls_read.argtypes = [C.c_void_p, C.POINTER(C.c_ubyte), C.c_size_t]; lib.hop_tls_read.restype = C.c_int
        lib.hop_tls_write.argtypes = [C.c_void_p, C.POINTER(C.c_ubyte), C.c_size_t]; lib.hop_tls_write.restype = C.c_int
        lib.hop_tls_error.argtypes = [C.c_void_p]; lib.hop_tls_error.restype = C.c_int
        lib.hop_tls_free.argtypes = [C.c_void_p]; lib.hop_tls_free.restype = None
        self.want_read = lib.hop_tls_want_read_error()
        self.want_write = lib.hop_tls_want_write_error()

    def certificate_fingerprint(self, certificate):
        """Calculate a certificate fingerprint; this does not verify a peer."""
        cert = _pem_bytes(certificate, "certificate")
        try: f = self.lib.hop_tls_certificate_fingerprint
        except AttributeError as exc: raise TLSError("TLS runtime lacks certificate fingerprint support") from exc
        f.argtypes = [C.c_char_p, C.c_size_t, C.c_void_p, C.c_size_t]
        f.restype = C.c_int
        output = C.create_string_buffer(65)
        if f(cert, len(cert), output, len(output)) != 0: raise ValueError("invalid certificate")
        fingerprint = output.value.decode("ascii")
        if not re.fullmatch(r"[0-9A-F]{64}", fingerprint): raise TLSError("invalid native fingerprint")
        return fingerprint

    def validate_identity(self, certificate, private_key):
        """Use the shared TLS implementation to validate a certificate/key pair."""
        cert = _pem_bytes(certificate, "certificate")
        key = _pem_bytes(private_key, "private key")
        try: f = self.lib.hop_tls_validate_identity
        except AttributeError as exc: raise TLSError("TLS runtime lacks identity validation") from exc
        f.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t]
        f.restype = C.c_int
        return f(cert, len(cert), key, len(key)) == 0

    def random_bytes(self, size):
        if type(size) is not int or not 1 <= size <= 32: raise ValueError("invalid random byte count")
        try: f = self.lib.hop_tls_random_bytes
        except AttributeError as exc: raise TLSError("TLS runtime lacks secure randomness") from exc
        f.argtypes = [C.c_void_p, C.c_size_t]; f.restype = C.c_int
        output = (C.c_ubyte * size)()
        if f(output, size) != 0: raise TLSError("secure random generation failed")
        return bytes(output)

    def generate_identity(self):
        """Generate a fresh identity; callers must store it securely immediately."""
        try: f = self.lib.hop_tls_generate_identity
        except AttributeError as exc: raise TLSError("TLS runtime lacks identity generation") from exc
        f.argtypes = [C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t]
        f.restype = C.c_int
        cert=C.create_string_buffer(65536);key=C.create_string_buffer(65536);fingerprint=C.create_string_buffer(65)
        try:
            if f(cert,len(cert),key,len(key),fingerprint,len(fingerprint))!=0:raise TLSError('identity generation failed')
            certificate,private_key=bytes(cert.value),bytes(key.value)
            expected=fingerprint.value.decode('ascii')
            if not self.validate_identity(certificate,private_key) or self.certificate_fingerprint(certificate)!=expected:
                raise TLSError('generated identity failed validation')
            return certificate,private_key
        finally:
            C.memset(cert,0,len(cert));C.memset(key,0,len(key));C.memset(fingerprint,0,len(fingerprint))


class SecureSocket:
    """Own one connected socket and one non-thread-safe TLS channel.

    Certificate pins must be independently verified before use. All methods
    run on the creating thread. The caller must not use the underlying socket
    directly after construction. close() sends no TLS close_notify because the
    current C ABI has no close-notify operation.
    """
    def __init__(self, library, connected_socket, *, client, own_certificate, private_key, peer):
        if not isinstance(library, TLSLibrary) or not isinstance(connected_socket, socket.socket):
            raise TypeError("TLS library and connected socket required")
        if type(client) is not bool or not isinstance(peer, VerifiedPeer):
            raise ValueError("explicit role and previously verified peer required")
        cert = _pem_bytes(own_certificate, "local certificate")
        key = _pem_bytes(private_key, "private key")
        try:
            local_der = ssl.PEM_cert_to_DER_cert(cert.decode("ascii").strip())
            local_fingerprint = hashlib.sha256(local_der).hexdigest()
            VerifiedPeer(cert, local_fingerprint)  # require exactly one certificate
        except (UnicodeDecodeError, ValueError) as exc:
            raise ValueError("invalid local certificate") from exc
        self._thread = threading.get_ident()
        self._library = library
        self._peer_fingerprint = peer.fingerprint
        self._local_fingerprint = local_fingerprint
        self._socket = connected_socket
        try:
            if self._socket.family in (socket.AF_INET, socket.AF_INET6):
                self._socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self._socket.setblocking(False)
        except OSError:
            self._socket.close()
            raise
        self._channel = None
        self._authenticated = False
        self._want = "read"
        self._error = 0

        @_SEND
        def send(_context, data, size):
            try:
                if not size: return 0
                count = self._socket.send(C.string_at(data, size))
                return count if count else -1
            except (BlockingIOError, InterruptedError):
                self._want = "write"; return library.want_write
            except Exception:
                return -1

        @_RECV
        def receive(_context, output, size):
            try:
                data = self._socket.recv(min(size, 65536))
                if data: C.memmove(output, data, len(data))
                return len(data)
            except (BlockingIOError, InterruptedError):
                self._want = "read"; return library.want_read
            except Exception:
                return -1

        self._send, self._recv = send, receive  # callbacks outlive native channel
        channel = library.lib.hop_tls_create(int(client), cert, key, peer.certificate,
                                             C.c_void_p(1), self._send, self._recv)
        if not channel:
            self._socket.close()
            raise TLSError("TLS initialization failed; identity or pin is invalid")
        self._channel = channel

    @property
    def authenticated(self): return self._authenticated and self._channel is not None
    @property
    def peer_fingerprint(self):
        return self._peer_fingerprint if self.authenticated else None
    @property
    def local_fingerprint(self):
        return self._local_fingerprint if self.authenticated else None
    @property
    def last_error(self): return self._error

    def _check(self):
        if threading.get_ident() != self._thread: raise RuntimeError("TLS channel must be used on its owning thread")
        if not self._channel: raise TLSError("TLS channel is closed")

    def _fatal(self, message):
        if self._channel: self._error = self._library.lib.hop_tls_error(self._channel)
        self.close()
        raise TLSError(message)

    def handshake_step(self):
        self._check()
        if self._authenticated: return True
        result = self._library.lib.hop_tls_handshake(self._channel)
        if result == 1:
            self._authenticated = True
            return True
        if result == 0: return False
        self._fatal("TLS handshake or peer authentication failed")

    def _wait(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0: return False
        read = [self._socket] if self._want == "read" else []
        write = [self._socket] if self._want == "write" else []
        try:
            r, w, errors = select.select(read, write, [self._socket], remaining)
        except (OSError, ValueError):
            self._fatal("TLS socket wait failed")
        if errors: self._fatal("TLS socket failed")
        return bool(r or w)

    def handshake(self, timeout=10.0):
        self._check()
        if not isinstance(timeout, (int, float)) or not 0 < timeout <= 120: raise ValueError("invalid TLS timeout")
        deadline = time.monotonic() + timeout
        while not self.handshake_step():
            if not self._wait(deadline): self._fatal("TLS handshake timed out")
        return True

    def write_all(self, payload, timeout=10.0):
        self._check()
        if not self._authenticated: raise TLSError("TLS is not authenticated")
        if not isinstance(payload, bytes) or len(payload) > _MAX_IO: raise ValueError("invalid TLS payload")
        if not isinstance(timeout, (int, float)) or not 0 < timeout <= 120: raise ValueError("invalid TLS timeout")
        deadline = time.monotonic() + timeout
        offset = 0
        while offset < len(payload):
            # Retain exactly the same native buffer and length across WANT_READ/
            # WANT_WRITE retries, as required by mbedTLS and the Hop C ABI.
            part = payload[offset:offset + 16384]
            buf = (C.c_ubyte * len(part)).from_buffer_copy(part)
            while True:
                result = self._library.lib.hop_tls_write(self._channel, buf, len(part))
                if result > 0:
                    if result > len(part): self._fatal("TLS returned an invalid write length")
                    offset += result
                    break
                if result in (self._library.want_read, self._library.want_write):
                    self._want = "read" if result == self._library.want_read else "write"
                    if not self._wait(deadline): self._fatal("TLS write timed out")
                    continue
                self._fatal("TLS write failed")
        return offset

    def read_some(self, max_bytes=65536, timeout=10.0):
        self._check()
        if not self._authenticated: raise TLSError("TLS is not authenticated")
        if type(max_bytes) is not int or not 1 <= max_bytes <= _MAX_IO: raise ValueError("invalid read size")
        if not isinstance(timeout, (int, float)) or not 0 < timeout <= 120: raise ValueError("invalid TLS timeout")
        deadline = time.monotonic() + timeout
        data = (C.c_ubyte * max_bytes)()
        while True:
            result = self._library.lib.hop_tls_read(self._channel, data, max_bytes)
            if result > 0:
                if result > max_bytes: self._fatal("TLS returned an invalid read length")
                return bytes(data[:result])
            if result in (self._library.want_read, self._library.want_write):
                self._want = "read" if result == self._library.want_read else "write"
                if not self._wait(deadline): return None
                continue
            if result == 0:
                self.close()
                raise EOFError("TLS peer closed the connection")
            self._fatal("TLS read failed")

    def close(self):
        if threading.get_ident() != self._thread: raise RuntimeError("TLS channel must be used on its owning thread")
        self._authenticated = False
        if self._channel:
            self._library.lib.hop_tls_free(self._channel)
            self._channel = None
        self._socket.close()

    def __enter__(self): return self
    def __exit__(self, *_): self.close()
