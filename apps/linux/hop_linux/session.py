"""Experimental pinned-TLS session boundary; application codec is injected.

Wire envelope follows core/include/hop/frame.h. This module deliberately does
not define control/input payload bytes or perform discovery/pairing. The codec
and input sink must be supplied by the integrator after the shared protocol is
agreed. All operations run on the SecureSocket owner thread.
"""
from __future__ import annotations
import struct

_HEADER = struct.Struct('!4sBBHIQQ')
_LIMITS = {1: 4096, 2: 4096, 3: 4096, 4: 65536}
_MAX_SEQUENCE = (1 << 64) - 1


class ProtocolError(RuntimeError):
    pass


def encode_frame(kind, sequence, epoch, payload):
    if (type(kind) is not int or kind not in _LIMITS or type(sequence) is not int or
        not 1 <= sequence <= _MAX_SEQUENCE or type(epoch) is not int or not 1 <= epoch <= _MAX_SEQUENCE or
        not isinstance(payload, bytes) or len(payload) > _LIMITS.get(kind, 0) or (kind == 4 and not payload)):
        raise ProtocolError('invalid frame')
    return _HEADER.pack(b'HOP2', 2, kind, 0, len(payload), sequence, epoch) + payload


class FrameDecoder:
    def __init__(self):
        self.buffer = bytearray(); self.last_sequence = 0; self.failed = False

    def feed(self, data):
        if self.failed or not isinstance(data, bytes): raise ProtocolError('invalid decoder state or input')
        # Bound buffered untrusted data. No allocation proportional to a claimed length.
        if len(data) > 1024 * 1024: self.failed = True; raise ProtocolError('read exceeds limit')
        self.buffer.extend(data); frames = []
        try:
            while len(self.buffer) >= _HEADER.size:
                magic, version, kind, reserved, length, seq, epoch = _HEADER.unpack_from(self.buffer)
                if (magic != b'HOP2' or version != 2 or kind not in _LIMITS or reserved or
                    length > _LIMITS[kind] or (kind == 4 and not length) or seq <= self.last_sequence or not epoch):
                    raise ProtocolError('invalid frame header')
                if len(self.buffer) < _HEADER.size + length: break
                payload = bytes(self.buffer[_HEADER.size:_HEADER.size + length]); del self.buffer[:_HEADER.size + length]
                self.last_sequence = seq; frames.append((kind, seq, epoch, payload))
            return frames
        except Exception:
            self.failed = True; self.buffer.clear(); raise

    def finish(self):
        if self.failed or self.buffer: self.failed = True; raise ProtocolError('truncated frame')
        self.failed = True


class Session:
    """Experimental serialized v2 session; transport is already authenticated.

    sink.enter(fields) must check screen, permissions and safely establish the
    requested held input state; inject(event, fields), release_all() and
    restore_local() must be non-blocking. No platform sink is wired yet.
    Caller must poll frequently on the owning thread. This does not implement
    queues, pairing, clipboard, timers independent of polling or reconnect.
    """
    def __init__(self, transport, codec, sink, role, *, platform=3, capabilities=0, clock=None, clipboard=None, clipboard_sender=None):
        import time
        if role not in ('host', 'receiver'): raise ValueError('explicit session role required')
        if not transport.authenticated: raise ProtocolError('authenticated TLS required')
        if platform not in (1,2,3,4) or type(capabilities) is not int or capabilities < 0 or capabilities & ~63: raise ValueError('invalid capabilities')
        if capabilities & 6 and (clipboard is None or capabilities & 6 & ~getattr(clipboard,'supported_capabilities',0)):
            raise ValueError('clipboard capability lacks a configured adapter')
        if clipboard is not None and not callable(getattr(clipboard,'barrier_ready',None)):
            raise ValueError('clipboard adapter requires a revocation barrier')
        if role=='receiver' and capabilities & 6 and any(not callable(getattr(clipboard,n,None)) for n in ('expect_initial_snapshot','initial_done','initial_processed')):
            raise ValueError('receiver clipboard requires initial snapshot handling')
        self.transport, self.codec, self.sink, self.role = transport, codec, sink, role
        self.platform, self.capabilities = platform, capabilities
        self.clipboard=clipboard
        self.clipboard_sender=clipboard_sender
        self.decoder = FrameDecoder(); self.sequence = 0; self.session_epoch = 0; self.epoch = 0
        self.phase = 'hello'; self.closed = False; self.screen_id = 0; self.peer_capabilities = 0
        self.peer_platform = 0
        self.buttons = 0; self.keys = set(); self._clock = clock or time.monotonic
        self.screens = ()
        self.focus_deadline = None
        self._barrier_deadline=None;self._release_ack=False
        self.pairing_deadline = None; self._pair_local = False; self._pair_remote = False
        self.pairing_confirmed = False
        self._pending_pairing = None
        self._clip_local_ready=False;self._clip_peer_ready=False;self._clip_paused=False
        self.last_receive = self.last_heartbeat = self._clock()

    def _send(self, kind, epoch, payload):
        if self.closed or not self.transport.authenticated: raise ProtocolError('session closed')
        if self.sequence == _MAX_SEQUENCE: raise ProtocolError('sequence exhausted')
        self.sequence += 1
        self.transport.write_all(encode_frame(kind, self.sequence, epoch, payload), timeout=.2)

    def _control(self, event, epoch, **fields): self._send(1, epoch, self.codec.encode_control(event, **fields))

    def _hello_complete(self):
        self.phase = 'pairing' if (self.peer_capabilities | self.capabilities) & 32 else 'local'
        if self.phase == 'pairing': self.pairing_deadline = self._clock()+120

    def confirm_pairing(self, pending):
        if self.closed or self.phase != 'pairing' or self._pair_local: raise ProtocolError('pairing confirmation unavailable')
        try:
            self._pending_pairing=pending
            pending.verify_session(self.transport)
            self._control('pair_confirm',self.session_epoch);self._pair_local=True
            if self._pair_remote:
                self.pairing_confirmed=True;self.phase='local';self.pairing_deadline=None
        except BaseException:self.close();raise

    def cancel_pairing(self):
        if self.closed or self.phase != 'pairing': raise ProtocolError('pairing unavailable')
        try:self._control('pair_cancel',self.session_epoch)
        finally:self.close()

    def start(self):
        import secrets
        if self.closed or self.role != 'host' or self.phase != 'hello' or self.session_epoch: raise ProtocolError('cannot start session')
        self.session_epoch = secrets.randbelow((1 << 63) - 1) + 1; self.epoch = self.session_epoch
        try: self._control('hello', self.session_epoch, role=1, platform=self.platform, capabilities=self.capabilities)
        except Exception: self.close(); raise

    def enter(self, screen_id, x=0, y=0, modifiers=0, buttons=0):
        if self.closed or self.role != 'host' or self.phase != 'local' or self.epoch == _MAX_SEQUENCE or not (self.peer_capabilities & self.capabilities & 1) or screen_id not in {screen[0] for screen in self.screens}:
            raise ProtocolError('cannot enter remote focus')
        self.epoch += 1; self.phase = 'entering'; self.screen_id = screen_id; self.focus_deadline = self._clock()+3
        try: self._control('enter', self.epoch, screen_id=screen_id, x=x, y=y, modifiers=modifiers, buttons=buttons)
        except Exception: self.close(); raise

    def announce_screens(self, screens):
        if self.closed or self.role != 'receiver' or self.phase != 'local': raise ProtocolError('cannot announce screens')
        payload = self.codec.encode_control('screens', screens=screens)
        self.screens = tuple(self.codec.decode_control(payload)[1]['screens'])
        try: self._send(1, self.session_epoch, payload)
        except Exception: self.close(); raise

    def release(self):
        if self.closed or self.role != 'host' or self.phase != 'remote': raise ProtocolError('remote focus required')
        self.phase = 'releasing'; self.focus_deadline = self._clock()+3
        self._barrier_deadline=self._clock()+2;self._release_ack=False
        try:
            self.sink.restore_local()
            self._focus_clipboard(0)
            self._reset_clipboard_gate()
            self._control('release', self.epoch, reason=0)
        except Exception: self.close(); raise

    def send_input(self, event, **fields):
        if self.closed or self.role != 'host' or self.phase != 'remote': raise ProtocolError('remote focus not acknowledged')
        try:
            payload = self.codec.encode_input(event, **fields)
            decoded, values = self.codec.decode_input(payload)
            if decoded in ('move','button') and values['screen_id'] != self.screen_id: raise ProtocolError('wrong active screen')
            negotiated = self.peer_capabilities & self.capabilities
            if decoded == 'wheel' and values['horizontal'] and not (negotiated & 8): raise ProtocolError('horizontal wheel not negotiated')
            if decoded == 'button' and values['button'] in (4,5) and not (negotiated & 16): raise ProtocolError('side buttons not negotiated')
            self._send(2, self.epoch, payload)
        except Exception: self.close(); raise

    def clipboard_send_ready(self):
        if (self.closed or self.phase!='remote' or not self.clipboard or self._clip_paused or
            not self._clip_local_ready or not self._clip_peer_ready):return False
        return self.role=='host' or (self._clip_initial_received and self.clipboard.initial_processed())

    def send_clipboard_frame(self, kind, payload):
        """Send one prepared frame; the caller schedules input before each chunk."""
        if not self.clipboard_send_ready():raise ProtocolError('clipboard actors not ready')
        try:
            if kind==3:
                event,fields=self.codec.decode_clipboard_control(payload)
                if event=='start':
                    bit=2 if fields['format']==1 else 4
                    if not (self.capabilities & self.peer_capabilities & bit):raise ProtocolError('clipboard format not negotiated')
                    if self._clip_outgoing is not None:raise ProtocolError('clipboard transfer already active')
                    if self.role=='host' and not self._clip_initial_sent:
                        if self._clip_initial_started:raise ProtocolError('multiple initial snapshots')
                        self._clip_initial_started=True
                    if fields['total']:self._clip_outgoing=[fields['transfer_id'],fields['total'],0]
                elif event=='cancel':
                    if self._clip_outgoing is None or fields['transfer_id']!=self._clip_outgoing[0]:raise ProtocolError('unknown outgoing transfer')
                    self._clip_outgoing=None
                elif event=='initial_done':
                    if self.role!='host' or self._clip_initial_sent or self._clip_outgoing is not None:
                        raise ProtocolError('invalid initial clipboard completion')
                    self._clip_initial_sent=True
            elif kind==4:
                _event,fields=self.codec.decode_clipboard_chunk(payload);entry=self._clip_outgoing
                if entry is None or fields['transfer_id']!=entry[0] or fields['offset']!=entry[2] or entry[2]+len(fields['data'])>entry[1]:
                    raise ProtocolError('invalid outgoing clipboard chunk')
                entry[2]+=len(fields['data'])
                if entry[2]==entry[1]:self._clip_outgoing=None
            else:raise ProtocolError('invalid clipboard frame')
            self._send(kind,self.epoch,payload)
        except Exception:self.close();raise

    def _reset_clipboard_gate(self):
        self._clip_local_ready=False;self._clip_peer_ready=False;self._clip_paused=False
        self._clip_initial_started=False;self._clip_initial_sent=False;self._clip_initial_received=False;self._clip_outgoing=None

    def _focus_clipboard(self,epoch):
        if self.clipboard:self.clipboard.focus(epoch)
        if self.clipboard_sender:self.clipboard_sender.focus(epoch)

    def _progress_release(self):
        if self.phase!='releasing':return
        if self._barrier_deadline is not None and self._clock()>=self._barrier_deadline:
            raise ProtocolError('clipboard revocation timed out')
        ready=not self.clipboard or self.clipboard.barrier_ready()
        if self._barrier_deadline is not None and self._clock()>=self._barrier_deadline:
            raise ProtocolError('clipboard revocation timed out')
        if not ready:
            if self._barrier_deadline is None:raise ProtocolError('clipboard barrier regressed')
            return
        self._barrier_deadline=None
        if self.role=='receiver':
            self._control('released',self.epoch);self.phase='local';self.focus_deadline=None
        elif self._release_ack:
            self.phase='local';self.focus_deadline=None

    def clipboard_ready(self):
        if (self.closed or self.phase!='remote' or not self.clipboard or self._clip_paused or
            self._clip_local_ready or not (self.capabilities & self.peer_capabilities & 6)):
            raise ProtocolError('clipboard actor cannot be marked ready')
        try:self._control('clipboard_ready',self.epoch);self._clip_local_ready=True
        except Exception:self.close();raise

    def clipboard_pause(self):
        if self.closed or self.phase!='remote' or not self.clipboard or self._clip_paused:
            raise ProtocolError('clipboard actor cannot be paused')
        self._clip_paused=True;self._clip_local_ready=False;self._clip_peer_ready=False
        try:
            self._focus_clipboard(0)
            self._control('clipboard_pause',self.epoch)
        except Exception:self.close();raise

    def _handle(self, kind, epoch, payload):
        if kind == 1:
            event, fields = self.codec.decode_control(payload)
            if self.phase == 'hello':
                if self.role == 'receiver' and event == 'hello' and fields['role'] == 1 and 0 < epoch < 2**63:
                    self.session_epoch = self.epoch = epoch; self.peer_capabilities = fields['capabilities']; self.peer_platform=fields['platform']
                    self._control('hello_ack', epoch, role=2, platform=self.platform, capabilities=self.capabilities); self._hello_complete(); return
                if self.role == 'host' and event == 'hello_ack' and fields['role'] == 2 and epoch == self.session_epoch and epoch:
                    self.peer_capabilities = fields['capabilities']; self.peer_platform=fields['platform']; self._hello_complete(); return
                raise ProtocolError('invalid hello transition')
            if event == 'heartbeat' and epoch == self.session_epoch: return
            if event == 'bye' and epoch == self.session_epoch: self.close(); return
            if self.phase == 'pairing':
                if epoch != self.session_epoch: raise ProtocolError('invalid pairing epoch')
                if event == 'pair_cancel': self.close(); return
                if event == 'pair_confirm' and not self._pair_remote:
                    self._pair_remote=True
                    if self._pair_local:
                        self.pairing_confirmed=True;self.phase='local';self.pairing_deadline=None
                    return
                raise ProtocolError('unexpected pairing message')
            if event in ('clipboard_ready','clipboard_pause') and (epoch<self.epoch or (epoch==self.epoch and self.phase!='remote')):
                return
            if event in ('clipboard_ready','clipboard_pause') and self.phase=='remote' and epoch==self.epoch and self.clipboard:
                if event=='clipboard_ready':
                    if self._clip_paused:return # READY may have been in flight before local PAUSE.
                    if self._clip_peer_ready or not (self.capabilities & self.peer_capabilities & 6):
                        raise ProtocolError('unexpected clipboard readiness')
                    self._clip_peer_ready=True;return
                if self._clip_paused:return # Simultaneous PAUSE is idempotent.
                self._clip_paused=True;self._clip_local_ready=False;self._clip_peer_ready=False
                self._focus_clipboard(0);return
            if self.role == 'host' and event == 'screens' and epoch == self.session_epoch:
                if self.phase != 'local': raise ProtocolError('screens changed during active focus')
                self.screens = tuple(fields['screens']); return
            if self.role == 'receiver' and event == 'enter' and self.phase == 'local' and epoch > self.epoch and self.peer_capabilities & self.capabilities & 1:
                if fields['screen_id'] not in {screen[0] for screen in self.screens}: raise ProtocolError('unknown screen')
                # Held-state handoff is not implemented by the X11 adapter yet.
                if fields['modifiers'] or fields['buttons']: raise ProtocolError('held-state handoff unsupported')
                self.sink.release_all()
                self._reset_clipboard_gate()
                self._focus_clipboard(epoch)
                if self.clipboard and self.capabilities & self.peer_capabilities & 6:self.clipboard.expect_initial_snapshot()
                self.sink.enter(fields); self.epoch = epoch; self.screen_id = fields['screen_id']
                self.buttons = fields['buttons']; self.keys.clear(); self.phase = 'remote'
                self._control('entered', epoch, screen_id=self.screen_id); return
            if self.role == 'receiver' and event == 'release' and self.phase == 'remote' and epoch == self.epoch:
                self.sink.release_all(); self.buttons = 0; self.keys.clear(); self.phase = 'releasing'
                self._focus_clipboard(0)
                self._reset_clipboard_gate()
                self._barrier_deadline=self._clock()+2;self._progress_release();return
            if self.role == 'host' and event == 'entered' and self.phase == 'entering' and epoch == self.epoch and fields['screen_id'] == self.screen_id:
                self.phase = 'remote'; self.focus_deadline = None
                self._reset_clipboard_gate()
                self._focus_clipboard(epoch)
                return
            if self.role == 'host' and event == 'released' and self.phase == 'releasing' and epoch == self.epoch:
                if self._release_ack:raise ProtocolError('duplicate release acknowledgment')
                self._release_ack=True;self._progress_release();return
            raise ProtocolError('unexpected focus transition or unsupported control')
        if kind == 2 and self.role == 'receiver' and self.phase == 'remote' and epoch == self.epoch:
            event, fields = self.codec.decode_input(payload)
            negotiated = self.peer_capabilities & self.capabilities
            if event == 'wheel' and fields['horizontal'] and not (negotiated & 8): raise ProtocolError('horizontal wheel not negotiated')
            if event == 'button' and fields['button'] in (4,5) and not (negotiated & 16): raise ProtocolError('side buttons not negotiated')
            if event in ('move','button') and fields['screen_id'] != self.screen_id: raise ProtocolError('wrong active screen')
            if event == 'button':
                bit = 1 << (fields['button'] - 1)
                expected = self.buttons | bit if fields['down'] else self.buttons & ~bit
                if fields['buttons'] != expected or bool(self.buttons & bit) == bool(fields['down']): raise ProtocolError('inconsistent button state')
                self.buttons = expected
            if event == 'key':
                usage, down, repeat = fields['usage'],fields['down'],fields['repeat']
                if (down and ((usage in self.keys) != bool(repeat))) or (not down and usage not in self.keys): raise ProtocolError('inconsistent key state')
                if down: self.keys.add(usage)
                else: self.keys.remove(usage)
            self.sink.inject(event, fields); return
        if kind in (3,4):
            # Even discarded frames must be structurally bounded and valid.
            parsed=self.codec.decode_clipboard_control(payload) if kind==3 else self.codec.decode_clipboard_chunk(payload)
            if epoch<self.epoch or (epoch==self.epoch and (self._clip_paused or self.phase!='remote')):return
            if (not self.clipboard or self.phase!='remote' or epoch!=self.epoch or
                not self._clip_local_ready or not self._clip_peer_ready):raise ProtocolError('clipboard actor not ready')
            if self.role=='host' and not self._clip_initial_sent:raise ProtocolError('initial clipboard snapshot not sent')
            if kind==3:
                event,fields=parsed
                if event=='start':
                    bit=2 if fields['format']==1 else 4
                    if not (self.capabilities & self.peer_capabilities & bit):raise ProtocolError('clipboard format not negotiated')
                    if self.role=='receiver' and not self._clip_initial_received:
                        if self._clip_initial_started:raise ProtocolError('multiple initial snapshots')
                        self._clip_initial_started=True
                if event=='initial_done':
                    if self.role!='receiver' or self._clip_initial_received:raise ProtocolError('unexpected initial clipboard completion')
                    self.clipboard.initial_done();self._clip_initial_received=True
                else:self.clipboard.control(epoch,event,fields)
            else:
                _event,fields=parsed;self.clipboard.chunk(epoch,fields)
            return
        raise ProtocolError('frame outside active focus or unsupported kind')

    def poll(self, timeout=.05):
        if self.closed: raise ProtocolError('session closed')
        try:
            self._progress_release()
            now = self._clock()
            if now - self.last_receive >= 3: raise ProtocolError('peer timed out')
            if self.focus_deadline is not None and now >= self.focus_deadline: raise ProtocolError('focus acknowledgment timed out')
            if self.pairing_deadline is not None and now >= self.pairing_deadline: raise ProtocolError('pairing timed out')
            if self.phase != 'hello' and now - self.last_heartbeat >= 1:
                self._control('heartbeat', self.session_epoch, nonce=0); self.last_heartbeat = now
            data = self.transport.read_some(timeout=min(timeout,.2))
            if data is None: return 0
            frames = self.decoder.feed(data)
            for kind, _sequence, epoch, payload in frames:
                self._handle(kind, epoch, payload); self.last_receive = self._clock()
                if self.closed: break
            return len(frames)
        except BaseException:
            self.close(); raise

    def close(self):
        if self.closed: return
        self.closed = True; self.phase = 'local'
        failures = []
        disable_clipboard = getattr(self.clipboard, 'disable_access', None)
        if disable_clipboard:
            try:disable_clipboard()
            except Exception as exc:failures.append(exc)
        if self._pending_pairing:
            try:self._pending_pairing.cancel()
            except Exception as exc:failures.append(exc)
            self._pending_pairing = None
        for action in (self.sink.restore_local, self.sink.release_all, self.transport.close):
            try: action()
            except Exception as exc: failures.append(exc)
        if self.clipboard:
            try:self.clipboard.close()
            except Exception as exc:failures.append(exc)
            self.clipboard=None
        if self.clipboard_sender:
            try:self.clipboard_sender.close()
            except Exception as exc:failures.append(exc)
            self.clipboard_sender=None
        if failures: raise RuntimeError('session cleanup incomplete') from failures[0]
