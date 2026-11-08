#!/usr/bin/env python3
"""Hop v1 reference codec + fake peer; standard library only, NEVER captures/injects input."""
from __future__ import annotations
import argparse
from dataclasses import dataclass
from enum import IntEnum
import hmac
import json
from pathlib import Path
import queue
import secrets
import select
import socket
import struct
import threading
import time
import uuid

SIZE = 128
WIRE_SIZE = 132
ZERO_TOKEN = bytes(32)
ZERO_NONCE = bytes(16)


class ProtocolError(ValueError):
    pass


class Type(IntEnum):
    HELLO = 1
    ENTER = 2
    LEAVE = 3
    REQUEST_LEAVE = 4
    MOUSE_MOVE = 5
    MOUSE_BUTTON = 6
    WHEEL = 7
    KEY = 8
    KEEPALIVE = 9
    BYE = 10


class Phase(IntEnum):
    RESUME = 0
    PAIR = 1
    CHALLENGE = 2
    CONFIRM = 3
    READY = 4
    COMMIT = 5
    REJECT = 6


FORMATS = {Type.ENTER: '>HHHH', Type.LEAVE: '>HHB', Type.REQUEST_LEAVE: '>HH',
           Type.MOUSE_MOVE: '>HH', Type.MOUSE_BUTTON: '>BBHHH', Type.WHEEL: '>ii',
           Type.KEY: '>HHBB', Type.KEEPALIVE: '>Q', Type.BYE: '>B'}


@dataclass(frozen=True)
class Hello:
    phase: Phase
    role: int
    direction: int
    code: int
    width: int
    height: int
    identity: bytes
    nonce: bytes
    token: bytes = ZERO_TOKEN
    name: str = 'Mock peer'

    def payload(self):
        name = self.name.encode('utf-8')
        if len(name) > 31 or b'\0' in name:
            raise ProtocolError('invalid UTF-8 name length/NUL')
        if len(self.identity) != 16 or len(self.nonce) != 16 or len(self.token) != 32:
            raise ProtocolError('invalid identity/nonce/token length')
        return struct.pack('>BBBBIII16s16s32s32s8s', self.phase, self.role, self.direction, 0,
                           self.code, self.width, self.height, self.identity, self.nonce,
                           self.token, name, bytes(8))

    @classmethod
    def parse(cls, payload):
        p, role, direction, reserved, code, width, height, identity, nonce, token, rawname, tail = struct.unpack('>BBBBIII16s16s32s32s8s', payload)
        try:
            phase = Phase(p)
        except ValueError as exc:
            raise ProtocolError('invalid hello phase') from exc
        if reserved or any(tail) or role not in (1, 2) or direction not in (0, 1):
            raise ProtocolError('invalid hello fields')
        if not (1 <= width <= 65535 and 1 <= height <= 65535):
            raise ProtocolError('invalid display dimensions')
        if identity == bytes(16) or nonce == ZERO_NONCE:
            raise ProtocolError('zero identity/nonce')
        if code != 0 and not 100000 <= code <= 999999:
            raise ProtocolError('invalid pairing code')
        if phase in (Phase.CHALLENGE, Phase.CONFIRM, Phase.COMMIT) and not code:
            raise ProtocolError('pairing phase requires code')
        if phase in (Phase.PAIR, Phase.CHALLENGE, Phase.CONFIRM, Phase.REJECT) and token != ZERO_TOKEN:
            raise ProtocolError('token disclosed before mutual confirmation')
        if phase in (Phase.RESUME, Phase.READY, Phase.COMMIT) and token == ZERO_TOKEN:
            raise ProtocolError('missing authentication token')
        before, nul, after = rawname.partition(b'\0')
        if not nul or any(after):
            raise ProtocolError('invalid name padding')
        try:
            name = before.decode('utf-8')
        except UnicodeDecodeError as exc:
            raise ProtocolError('invalid UTF-8') from exc
        return cls(phase, role, direction, code, width, height, identity, nonce, token, name)


@dataclass(frozen=True)
class Frame:
    type: Type
    epoch: int = 0
    payload: bytes = b''
    flags: int = 0

    def body(self):
        if len(self.payload) > 120:
            raise ProtocolError('payload exceeds fixed body')
        try:
            header = struct.pack('>BBBBI', 1, self.type, self.flags, 0, self.epoch)
        except (struct.error, OverflowError) as exc:
            raise ProtocolError('invalid frame header') from exc
        body = header + self.payload.ljust(120, b'\0')
        self.from_body(body)
        return body

    def encode(self):
        return struct.pack('>I', SIZE) + self.body()

    @classmethod
    def from_body(cls, body):
        if len(body) != SIZE:
            raise ProtocolError('invalid fixed body length')
        version, rawtype, flags, reserved, epoch = struct.unpack_from('>BBBBI', body)
        if version != 1 or reserved:
            raise ProtocolError('invalid version/reserved')
        try:
            typ = Type(rawtype)
        except ValueError as exc:
            raise ProtocolError('unknown type') from exc
        if flags not in ((0, 1) if typ == Type.HELLO else (0,)):
            raise ProtocolError('invalid flags')
        if (typ in (Type.HELLO, Type.KEEPALIVE, Type.BYE)) != (epoch == 0):
            raise ProtocolError('invalid epoch')
        payload = body[8:]
        if typ == Type.HELLO:
            hello = Hello.parse(payload)
            if flags != int(hello.role == 1):
                raise ProtocolError('HELLO ACK/role mismatch')
        else:
            fmt = FORMATS[typ]
            length = struct.calcsize(fmt)
            if any(payload[length:]):
                raise ProtocolError('nonzero unused payload')
            fields = struct.unpack(fmt, payload[:length])
            if typ == Type.ENTER and (fields[2] & ~511 or fields[3] & ~31):
                raise ProtocolError('invalid enter input masks')
            if typ == Type.LEAVE and fields[2] > 2:
                raise ProtocolError('invalid leave reason')
            if typ == Type.MOUSE_BUTTON:
                button, action, _, _, buttons = fields
                if button not in range(1, 6) or action not in (0, 1) or buttons & ~31:
                    raise ProtocolError('invalid mouse button')
                if bool(buttons & (1 << (button - 1))) != bool(action):
                    raise ProtocolError('button state mismatch')
            if typ == Type.WHEEL and any(abs(v) > 120000 for v in fields):
                raise ProtocolError('wheel out of bounds')
            if typ == Type.KEY:
                key, mods, action, repeat = fields
                if key >= 128 or mods & ~511 or action not in (0, 1) or repeat not in (0, 1) or (not action and repeat):
                    raise ProtocolError('invalid key event')
            if typ == Type.BYE and fields[0] > 4:
                raise ProtocolError('invalid bye reason')
        return cls(typ, epoch, payload, flags)

    def fields(self):
        if self.type == Type.HELLO:
            return Hello.parse(self.payload.ljust(120, b'\0'))
        fmt = FORMATS[self.type]
        return struct.unpack(fmt, self.payload[:struct.calcsize(fmt)])


def message(typ, *values, epoch=0):
    return Frame(typ, epoch, struct.pack(FORMATS[typ], *values))


def greeting(hello):
    return Frame(Type.HELLO, payload=hello.payload(), flags=int(hello.role == 1))


class Decoder:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        result = []
        while len(self.buffer) >= 4:
            if struct.unpack_from('>I', self.buffer)[0] != SIZE:
                self.buffer.clear()
                raise ProtocolError('length prefix must be exactly 128')
            if len(self.buffer) < WIRE_SIZE:
                break
            body = bytes(self.buffer[4:WIRE_SIZE])
            del self.buffer[:WIRE_SIZE]
            result.append(Frame.from_body(body))
        return result

    def eof(self):
        if self.buffer:
            raise ProtocolError('EOF inside a frame')


@dataclass
class Binding:
    identity: bytes
    token: bytes


class PairServer:
    """Pure deterministic handshake state machine; persistence occurs only at commit ACK."""
    def __init__(self, identity, direction=1, binding=None, clock=time.monotonic):
        self.identity, self.direction, self.binding = identity, direction, binding
        self.clock = clock
        self.candidate = None
        self.reset()

    def reset(self):
        self.peer = None
        self.nonce = ZERO_NONCE
        self.code = 0
        self.token = ZERO_TOKEN
        self.client_confirmed = self.local_confirmed = self.ready = False
        self.committing = False
        self.started = self.clock()

    def reply(self, phase):
        token = self.token if phase in (Phase.COMMIT, Phase.READY) else ZERO_TOKEN
        return greeting(Hello(phase, 1, self.direction, self.code, 1920, 1080,
                              self.identity, self.nonce, token, 'Mock Mac'))

    def maybe_commit(self):
        if self.local_confirmed and self.client_confirmed and not self.committing:
            self.committing = True
            self.candidate = (self.peer, self.token, self.code, self.clock() + 60)
            return self.reply(Phase.COMMIT)
        return None

    def confirm(self):
        if self.peer is None or self.ready or self.clock() - self.started >= 60:
            raise ProtocolError('no live pairing challenge')
        self.local_confirmed = True
        return self.maybe_commit()

    def receive(self, frame):
        if frame.type != Type.HELLO or frame.flags:
            raise ProtocolError('client HELLO required')
        hello = frame.fields()
        if hello.role != 2:
            raise ProtocolError('client role required')
        if self.ready or self.clock() - self.started >= 60:
            raise ProtocolError('handshake closed/expired')
        if self.peer is None:
            self.peer, self.nonce = hello.identity, hello.nonce
            if hello.phase == Phase.RESUME:
                candidate = self.candidate
                valid_candidate = candidate and candidate[3] > self.clock() and hmac.compare_digest(candidate[0], self.peer) and hmac.compare_digest(candidate[1], hello.token)
                bound = self.binding
                valid_bound = bound and hmac.compare_digest(bound.identity, self.peer) and hmac.compare_digest(bound.token, hello.token)
                if not (valid_bound or valid_candidate):
                    return self.reply(Phase.REJECT)
                self.token = hello.token
                if valid_candidate and not valid_bound:
                    self.binding = Binding(self.peer, self.token)
                self.ready = True
                return self.reply(Phase.READY)
            if hello.phase != Phase.PAIR or self.binding is not None:
                return self.reply(Phase.REJECT)
            self.code = 100000 + secrets.randbelow(900000)
            self.token = secrets.token_bytes(32)
            return self.reply(Phase.CHALLENGE)
        if not hmac.compare_digest(hello.identity, self.peer) or not hmac.compare_digest(hello.nonce, self.nonce) or hello.code != self.code or hello.direction != self.direction:
            raise ProtocolError('identity/nonce/code/direction mismatch')
        if hello.phase == Phase.CONFIRM:
            self.client_confirmed = True
            return self.maybe_commit()
        if hello.phase == Phase.COMMIT and self.committing and hmac.compare_digest(hello.token, self.token):
            self.binding = Binding(self.peer, self.token)
            self.candidate = None
            self.ready = True
            return self.reply(Phase.READY)
        raise ProtocolError('unexpected handshake phase/token')


class PairClient:
    def __init__(self, identity, binding=None, clock=time.monotonic):
        self.identity, self.binding = identity, binding
        self.clock, self.started = clock, clock()
        self.nonce = secrets.token_bytes(16)
        self.peer = None
        self.code = 0
        self.direction = 1
        self.ready = self.confirmed = False

    def send(self, phase, token=ZERO_TOKEN):
        return greeting(Hello(phase, 2, self.direction, self.code, 1920, 1080, self.identity,
                              self.nonce, token, 'Mock Harmony PC'))

    def start(self):
        return self.send(Phase.RESUME, self.binding.token) if self.binding else self.send(Phase.PAIR)

    def confirm(self):
        if not self.peer or not self.code or self.confirmed:
            raise ProtocolError('no new challenge')
        self.confirmed = True
        return self.send(Phase.CONFIRM)

    def receive(self, frame):
        if frame.type != Type.HELLO or frame.flags != 1:
            raise ProtocolError('server ACK required')
        h = frame.fields()
        if self.ready or self.clock() - self.started >= 60:
            raise ProtocolError('handshake closed/expired')
        if not hmac.compare_digest(h.nonce, self.nonce) or (self.peer and not hmac.compare_digest(h.identity, self.peer)):
            raise ProtocolError('server identity/nonce mismatch')
        if h.phase == Phase.REJECT:
            raise ProtocolError('authentication rejected; unpair explicitly, never downgrade')
        if h.phase == Phase.CHALLENGE and not self.binding and not self.peer:
            self.peer, self.code, self.direction = h.identity, h.code, h.direction
            return None
        if h.phase == Phase.COMMIT and self.confirmed and h.code == self.code:
            self.binding = Binding(h.identity, h.token)
            return self.send(Phase.COMMIT, h.token)
        if h.phase == Phase.READY and self.binding and hmac.compare_digest(h.identity, self.binding.identity) and hmac.compare_digest(h.token, self.binding.token):
            self.peer, self.direction = h.identity, h.direction
            self.ready = True
            return None
        raise ProtocolError('unexpected ACK phase/token/code')


class RemoteState:
    """Fake receiver (records only); enforces input authentication, epoch and release invariants."""
    def __init__(self, direction=1):
        self.direction = direction
        self.authenticated = self.authorized = False
        self.epoch = 0
        self.last_epoch = 0
        self.mode = 'local'
        self.keys = set()
        self.buttons = set()
        self.position = (0, 0)
        self.armed = False
        self.events = []

    def release(self):
        self.events += [('release_key', k) for k in sorted(self.keys)]
        self.events += [('release_button', b) for b in sorted(self.buttons)]
        self.keys.clear()
        self.buttons.clear()
        self.mode = 'local'
        self.epoch = 0
        self.armed = False

    def receive(self, frame):
        if frame.type in (Type.HELLO, Type.KEEPALIVE):
            return None
        if frame.type == Type.BYE:
            self.release()
            return None
        if not self.authenticated or not self.authorized:
            self.release()
            raise ProtocolError('input before authentication/authorization')
        if frame.type == Type.ENTER:
            # Serial number arithmetic permits wrap from 0xffffffff to 1.
            distance = (frame.epoch - self.last_epoch) & 0xffffffff
            if self.mode != 'local' or not distance or distance >= 0x80000000:
                raise ProtocolError('stale/reentrant ENTER')
            self.epoch = self.last_epoch = frame.epoch
            x, y, mods, buttons = frame.fields()
            self.position = (x, y)
            self.keys = {k for bit, k in enumerate((56, 60, 59, 62, 58, 61, 55, 54)) if mods & (1 << bit)}
            self.buttons = {i + 1 for i in range(5) if buttons & (1 << i)}
            self.mode = 'remote'
            return message(Type.ENTER, x, y, mods, buttons, epoch=frame.epoch)
        if frame.epoch != self.epoch or not self.epoch:
            raise ProtocolError('stale input epoch')
        if frame.type == Type.LEAVE:
            self.release()
            return None
        if self.mode != 'remote':
            raise ProtocolError('input outside remote state')
        vals = frame.fields()
        if frame.type == Type.KEY:
            key, _, down, _ = vals
            (self.keys.add if down else self.keys.discard)(key)
        elif frame.type == Type.MOUSE_BUTTON:
            button, down, x, y, _ = vals
            self.position = (x, y)
            (self.buttons.add if down else self.buttons.discard)(button)
        elif frame.type == Type.MOUSE_MOVE:
            self.position = vals
            x, y = vals
            # 4px at this fake receiver's 1920px width, only after moving inside.
            if (self.direction == 1 and x >= 137) or (self.direction == 0 and x <= 65398):
                self.armed = True
            if self.armed and ((self.direction == 1 and x == 0) or (self.direction == 0 and x == 65535)):
                epoch = self.epoch
                self.release()
                self.epoch = epoch
                self.mode = 'leaving'
                return message(Type.REQUEST_LEAVE, x, y, epoch=epoch)
        elif frame.type not in (Type.WHEEL,):
            raise ProtocolError('unexpected receiver type')
        self.events.append((frame.type.name, vals))
        return None


class Coalescer:
    def __init__(self):
        self.pending = None

    def move(self, frame):
        if frame.type != Type.MOUSE_MOVE:
            raise ProtocolError('coalescer accepts MOVE only')
        self.pending = frame

    def flush(self, next_frame=None):
        result = [self.pending] if self.pending else []
        self.pending = None
        if next_frame:
            result.append(next_frame)
        return result

    def clear(self):
        self.pending = None


class Watchdog:
    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.last_rx = self.last_tx = clock()

    def received(self):
        self.last_rx = self.clock()

    def heartbeat_due(self):
        return self.clock() - self.last_tx >= 1

    def sent_heartbeat(self):
        self.last_tx = self.clock()

    def expired(self):
        return self.clock() - self.last_rx >= 3


def load_binding(path):
    if not path or not path.exists():
        return None
    data = json.loads(path.read_text())
    return Binding(bytes.fromhex(data['identity']), bytes.fromhex(data['token']))


def save_binding(path, binding):
    if path and binding:
        # Fake-peer local fixture only. Real apps MUST use Keychain / Preferences.
        import os
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, 'w') as file:
            json.dump({'identity': binding.identity.hex(), 'token': binding.token.hex()}, file)


def run_peer(sock, args, server):
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(0.5)  # bound writes; recovery never waits indefinitely
    binding_path = Path(args.binding) if args.binding else None
    bound = load_binding(binding_path)
    machine = PairServer(uuid.UUID(args.identity).bytes, args.direction, bound) if server else PairClient(uuid.UUID(args.identity).bytes, bound)
    remote = RemoteState(args.direction)
    decoder, watchdog = Decoder(), Watchdog()
    confirm = queue.SimpleQueue()
    prompt_started = False
    ready_at = None
    sent_exercise = False
    entered = False
    epoch = 1

    def send(frame):
        if frame:
            sock.sendall(frame.encode())
            print('TX', frame.type.name, 'epoch', frame.epoch, flush=True)

    def confirm_prompt():
        try:
            if input('Confirm matching six-digit code on BOTH panels? [yes/NO] ').strip() == 'yes':
                confirm.put(True)
            else:
                confirm.put(False)
        except EOFError:
            confirm.put(False)

    if not server:
        send(machine.start())
    try:
        while True:
            now = time.monotonic()
            if watchdog.expired():
                raise TimeoutError('3 second heartbeat timeout')
            if not machine.ready and now - machine.started >= 60:
                raise TimeoutError('60 second pairing timeout')
            if watchdog.heartbeat_due():
                send(message(Type.KEEPALIVE, int(now * 1000)))
                watchdog.sent_heartbeat()
            if machine.code and not prompt_started and not machine.ready:
                print(f'PAIR CODE {machine.code:06d} (test fixture, not a secure channel)', flush=True)
                prompt_started = True
                if args.auto_confirm:
                    confirm.put(True)
                else:
                    threading.Thread(target=confirm_prompt, daemon=True).start()
            try:
                confirmed = confirm.get_nowait()
            except queue.Empty:
                confirmed = None
            if confirmed is False:
                send(message(Type.BYE, 0))
                return
            if confirmed:
                send(machine.confirm())
            if machine.ready and ready_at is None:
                ready_at = now
                remote.authenticated = remote.authorized = True
                print('AUTHENTICATED (mock never injects physical input)', flush=True)
                save_binding(binding_path, machine.binding)
            if args.duration and ready_at and now - ready_at >= args.duration:
                send(message(Type.BYE, 0))
                return
            if server and args.exercise and ready_at and not sent_exercise:
                sent_exercise = True
                send(message(Type.ENTER, 70 if args.direction == 1 else 65465, 32768, 0, 0, epoch=epoch))
            readable, _, _ = select.select([sock], [], [], .01)
            if not readable:
                continue
            data = sock.recv(4096)
            if not data:
                decoder.eof()
                return
            for frame in decoder.feed(data):
                watchdog.received()
                print('RX', frame.type.name, 'epoch', frame.epoch, flush=True)
                if frame.type == Type.HELLO:
                    send(machine.receive(frame))
                    if not server:
                        remote.direction = machine.direction
                    # READY and ENTER may arrive in ONE recv(). Activate the fake
                    # receiver here, not at the next outer-loop iteration.
                    if machine.ready:
                        remote.authenticated = remote.authorized = True
                elif frame.type == Type.BYE:
                    if frame.fields()[0] == 4 and machine.ready:
                        print('Peer unpaired; remove --binding fixture explicitly before pairing again', flush=True)
                    return
                elif frame.type == Type.KEEPALIVE:
                    pass
                elif not machine.ready:
                    raise ProtocolError('input before handshake')
                elif not server:
                    send(remote.receive(frame))
                elif frame.type == Type.REQUEST_LEAVE:
                    send(message(Type.LEAVE, *frame.fields(), 0, epoch=frame.epoch))
                    print('RETURNED to mock Mac', flush=True)
                elif frame.type == Type.ENTER and not entered:
                    entered = True
                    # Synthetic test only, opt-in --exercise. No captures or text entry.
                    for f in (message(Type.MOUSE_MOVE, 32768, 32768, epoch=epoch),
                              message(Type.MOUSE_BUTTON, 1, 1, 32768, 32768, 1, epoch=epoch),
                              message(Type.MOUSE_MOVE, 40000, 35000, epoch=epoch),
                              message(Type.MOUSE_BUTTON, 1, 0, 40000, 35000, 0, epoch=epoch),
                              message(Type.WHEEL, 0, 120, epoch=epoch),
                              message(Type.MOUSE_MOVE, 0 if args.direction == 1 else 65535, 32768, epoch=epoch)):
                        send(f)
                else:
                    raise ProtocolError('unexpected server-side input')
    finally:
        remote.release()
        sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('role', choices=('server', 'client'))
    parser.add_argument('--host', default='127.0.0.1', help='explicit LAN IP for LAN testing; default loopback')
    parser.add_argument('--port', type=int, default=24890)
    parser.add_argument('--identity', default='11111111-1111-4111-8111-111111111111')
    parser.add_argument('--direction', type=int, choices=(0, 1), default=1)
    parser.add_argument('--binding', help='mock-only secret fixture JSON (chmod0600), not app storage')
    parser.add_argument('--auto-confirm', action='store_true', help='TEST ONLY; bypass this mock panel confirmation, NEVER use in app')
    parser.add_argument('--exercise', action='store_true', help='server: opt-in synthetic mouse sequence AFTER authentication/ENTER ACK')
    parser.add_argument('--duration', type=float, default=0, help='exit seconds after ready; zero = until disconnected')
    args = parser.parse_args()
    if args.role == 'client' and args.identity == '11111111-1111-4111-8111-111111111111':
        args.identity = '22222222-2222-4222-8222-222222222222'
    try:
        if args.role == 'server':
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind((args.host, args.port))
                listener.listen(1)
                print(f'Mock Mac listening on {args.host}:{args.port}', flush=True)
                sock, _ = listener.accept()
                run_peer(sock, args, True)
        else:
            run_peer(socket.create_connection((args.host, args.port), timeout=5), args, False)
    except (ProtocolError, TimeoutError, OSError, KeyboardInterrupt) as exc:
        parser.exit(1, f'Closed safely: {exc}\n')


if __name__ == '__main__':
    main()
