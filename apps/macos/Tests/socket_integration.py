#!/usr/bin/env python3
"""Real Swift Network.framework ↔ canonical Python codec, loopback only; no input APIs."""
import importlib.util
import pathlib
import select
import socket
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/mock-peer"))
import hop


def line(process, timeout=10):
    ready, _, _ = select.select([process.stdout], [], [], timeout)
    if not ready:
        raise AssertionError("headless Swift process output timed out")
    result = process.stdout.readline().strip()
    if not result:
        raise AssertionError(f"headless Swift exited unexpectedly: {process.poll()}")
    return result


def exact(sock, length):
    data = b""
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise EOFError("peer EOF")
        data += chunk
    return data


def receive(sock):
    prefix = exact(sock, 4)
    assert prefix == b"\x00\x00\x00\x80"
    return hop.Frame.from_body(exact(sock, 128))


def meaningful(sock):
    while True:
        frame = receive(sock)
        if frame.type != hop.Type.KEEPALIVE:
            return frame


def closed(sock, reason=None, timeout=4):
    sock.settimeout(timeout)
    seen = None
    try:
        while True:
            frame = receive(sock)
            if frame.type == hop.Type.BYE:
                seen = frame.fields()[0]
    except (EOFError, ConnectionResetError):
        pass
    if reason is not None:
        assert seen == reason, (seen, reason)
    sock.close()


def main():
    process = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, bufsize=1)
    sockets = []
    try:
        listener = line(process)
        assert listener.startswith("LISTEN "), listener
        port = int(listener.split()[1])
        peer = bytes([0x22]) * 16
        nonce = bytes(range(16))
        def connect():
            sock = socket.create_connection(("127.0.0.1", port), timeout=4)
            sock.settimeout(4); sockets.append(sock); return sock
        def hello(phase, code=0, token=bytes(32), identity=peer, current_nonce=nonce):
            return hop.greeting(hop.Hello(phase, 2, 1, code, 1920, 1080, identity, current_nonce, token, "Python test"))
        def command(value):
            process.stdin.write(value + "\n"); process.stdin.flush(); return line(process)

        unauthorized = connect()
        unauthorized.sendall(hop.message(hop.Type.MOUSE_MOVE, 1, 2, epoch=1).encode())
        closed(unauthorized, reason=1)
        print("PASS socket: unauthenticated input rejected")

        sock = connect()
        payload = hello(hop.Phase.PAIR).encode()
        for byte in payload:
            sock.sendall(bytes([byte]))
        challenge_frame = meaningful(sock)
        challenge = challenge_frame.fields()
        assert challenge_frame.flags == 1 and challenge.phase == hop.Phase.CHALLENGE
        assert 100000 <= challenge.code <= 999999 and challenge.token == bytes(32)
        assert challenge.nonce == nonce and challenge.identity == bytes([0x11]) * 16
        assert command("status") == "STATE unauthenticated unbound"
        # Remote confirmation first, repeated: MUST NOT expose a token or persist yet.
        sock.sendall(hello(hop.Phase.CONFIRM, challenge.code).encode() * 2)
        sock.settimeout(0.2)
        try:
            frame = receive(sock)
            assert frame.type == hop.Type.KEEPALIVE, "COMMIT before local confirmation"
        except socket.timeout:
            pass
        assert command("status") == "STATE unauthenticated unbound"
        assert command("confirm") == "CONFIRMED"
        sock.settimeout(4)
        commit = meaningful(sock).fields()
        assert commit.phase == hop.Phase.COMMIT and commit.code == challenge.code
        assert commit.nonce == nonce and commit.token != bytes(32)
        assert command("status") == "STATE unauthenticated unbound"
        sock.sendall(hello(hop.Phase.COMMIT, commit.code, commit.token).encode())
        ready = meaningful(sock).fields()
        assert ready.phase == hop.Phase.READY and ready.token == commit.token and ready.nonce == nonce
        assert command("status") == "STATE authenticated bound"
        print("PASS socket: byte-fragmented PAIR, double confirmation, commit acknowledgement, READY")

        second = connect()
        closed(second, timeout=2)
        sock.sendall(hop.message(hop.Type.KEEPALIVE, 100).encode())
        assert command("status") == "STATE authenticated bound"
        print("PASS socket: only one peer accepted")
        sock.sendall(hop.message(hop.Type.BYE, 0).encode())
        closed(sock, reason=0)

        resumed = connect(); fresh = bytes([0x71]) * 16
        resumed.sendall(hello(hop.Phase.RESUME, token=commit.token, current_nonce=fresh).encode())
        resumed_ready = meaningful(resumed).fields()
        assert resumed_ready.phase == hop.Phase.READY and resumed_ready.nonce == fresh
        print("PASS socket: token/UUID reconnect with fresh nonce")
        start = time.monotonic()
        closed(resumed, reason=2, timeout=5)
        elapsed = time.monotonic() - start
        assert 2.8 <= elapsed <= 4.5, elapsed
        print("PASS socket: monotonic3s receive timeout despite outgoing heartbeat")

        wrong = connect(); wrong.sendall(hello(hop.Phase.RESUME, token=bytes([0x99]) * 32).encode())
        closed(wrong, reason=1)
        downgrade = connect(); downgrade.sendall(hello(hop.Phase.PAIR).encode())
        closed(downgrade, reason=1)
        print("PASS socket: wrong token rejected and bound peer cannot silently downgrade to PAIR")

        malformed = connect(); malformed.sendall(b"\xff\xff\xff\xff")
        closed(malformed, reason=2)
        print("PASS socket: oversized frame length rejected before body allocation")
        short = connect(); short.sendall(hello(hop.Phase.RESUME, token=commit.token).encode()[:19]); short.shutdown(socket.SHUT_WR)
        closed(short, reason=2)
        print("PASS socket: truncated frame EOF closes cleanly")

        process.stdin.write("quit\n"); process.stdin.flush()
        assert process.wait(timeout=5) == 0
        stderr = process.stderr.read()
        if stderr:
            print("Swift transport diagnostics:", stderr.strip())
        print("PASS: 8 real loopback Swift/Python transport/session scenarios; no app, Keychain writes or input capture")
    finally:
        for sock in sockets:
            sock.close()
        if process.poll() is None:
            process.kill(); process.wait(timeout=5)


if __name__ == "__main__":
    main()
