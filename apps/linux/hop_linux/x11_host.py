"""Explicit, experimental X11 host capture with an independent fail-safe.

No capture is started by importing this module. The controller must establish
and ACK focus first and must ensure no physical keys/buttons are held at entry.
Unsupported keys, queue overflow, stalled pumping and capture errors fail closed.
"""
from collections import deque
import threading
import time

from .keymap import HeldKeyTranslator
from .profiles import PointerProfile
from .x11 import X11Backend

_MOD_BITS = {0xE1:0,0xE5:1,0xE0:2,0xE4:3,0xE2:4,0xE6:5,0xE3:6,0xE7:7}
_BUTTONS = {1:1,3:2,2:3,8:4,9:5}


class X11Host:
    def __init__(self, *, screen_id, target_size, capture_center, initial=(32768,32768),
                 profile=None, backend_factory=X11Backend, watchdog=1.0, edge_directions=()):
        if type(screen_id) is not int or not 0<screen_id<2**64: raise ValueError('invalid screen')
        if (not isinstance(target_size,tuple) or len(target_size)!=2 or
            any(type(v) is not int or not 1<=v<=32768 for v in target_size) or
            not isinstance(capture_center,tuple) or len(capture_center)!=2 or
            any(type(v) is not int or not 0<=v<=32767 for v in capture_center) or
            not isinstance(initial,tuple) or len(initial)!=2 or
            any(type(v) is not int or not 0<=v<=65535 for v in initial)):
            raise ValueError('invalid capture geometry')
        if not isinstance(watchdog,(int,float)) or not .1<=watchdog<=3: raise ValueError('invalid watchdog')
        self.screen_id,self.target_size,self.center=screen_id,target_size,capture_center
        self.pos=[initial[0]*(target_size[0]-1)/65535,initial[1]*(target_size[1]-1)/65535]
        self.profile=profile or PointerProfile()
        if not isinstance(self.profile,PointerProfile): raise ValueError('invalid profile')
        self.factory=backend_factory;self.watchdog=watchdog;self.queue=deque();self.lock=threading.Lock()
        if not isinstance(edge_directions,(tuple,list)) or any(d not in ('left','right','up','down') for d in edge_directions):raise ValueError('invalid edge directions')
        self.edge_directions=frozenset(edge_directions)
        self.stop=threading.Event();self.started=threading.Event();self.thread=None;self.error=None
        self.last_pump=time.monotonic();self.buttons=0;self.mods=0

    def start(self, *, acknowledged, input_state_clear):
        if acknowledged is not True or input_state_clear is not True or self.thread is not None:
            raise RuntimeError('acknowledged focus and explicitly clear local inputs required')
        self.last_pump=time.monotonic()
        self.thread=threading.Thread(target=self._run,daemon=True,name='hop-x11-capture');self.thread.start()
        if not self.started.wait(2): self.restore_local();raise RuntimeError('capture did not start')
        if self.error: self.restore_local();raise RuntimeError('capture failed') from self.error

    def _push(self, event, fields):
        with self.lock:
            if event=='move' and self.queue and self.queue[-1][0]=='move': self.queue[-1]=(event,fields);return
            if len(self.queue)>=256: raise RuntimeError('input queue full')
            self.queue.append((event,fields))

    def _coords(self):
        return tuple(round(self.pos[i]*65535/(self.target_size[i]-1)) if self.target_size[i]>1 else 0 for i in (0,1))

    def _run(self):
        backend=None
        try:
            backend=self.factory(); keys=HeldKeyTranslator(backend.keymap());backend.grab()
            if not backend.input_state_clear(): raise RuntimeError('local keys or buttons are held')
            backend.warp(*self.center)
            self.started.set()
            pending_motion=None;last_motion=0.0;last_edge=None
            def flush_motion():
                nonlocal pending_motion,last_motion
                if pending_motion is not None:
                    self._push('move',pending_motion);pending_motion=None;last_motion=time.monotonic()
            while not self.stop.is_set():
                if time.monotonic()-self.last_pump>self.watchdog: raise RuntimeError('host pump stalled')
                for event in backend.events():
                    if event.kind=='motion':
                        if (event.x,event.y)==self.center: continue
                        dx,dy=self.profile.move(event.x-self.center[0],event.y-self.center[1])
                        raw=(self.pos[0]+dx,self.pos[1]+dy)
                        direction=('left' if dx<0 and raw[0]<0 else 'right' if dx>0 and raw[0]>self.target_size[0]-1 else
                                   'up' if dy<0 and raw[1]<0 else 'down' if dy>0 and raw[1]>self.target_size[1]-1 else None)
                        for i,value in enumerate(raw):self.pos[i]=max(0,min(self.target_size[i]-1,value))
                        x,y=self._coords();pending_motion=dict(screen_id=self.screen_id,x=x,y=y);backend.warp(*self.center)
                        if direction in self.edge_directions and direction!=last_edge:
                            flush_motion();self._push('edge',dict(direction=direction,x=x,y=y,clear=not keys.held and not self.buttons));last_edge=direction
                        elif direction is None:last_edge=None
                    elif event.kind=='key':
                        flush_motion() # never reorder movement after a key/button
                        repeat=event.down and event.code in keys.held
                        usage=keys.press(event.code) if event.down else keys.release(event.code)
                        if usage is None: raise RuntimeError('unsupported or inconsistent key')
                        if usage in _MOD_BITS:
                            bit=1<<_MOD_BITS[usage];self.mods=(self.mods|bit) if event.down else (self.mods&~bit)
                        if usage==0x29 and event.down and self.mods&3 and self.mods&12 and self.mods&48:
                            raise RuntimeError('emergency local input restore')
                        mods=self.mods|(256 if event.state&2 else 0)
                        self._push('key',dict(usage=usage,modifiers=mods,down=int(event.down),repeat=int(repeat)))
                    elif event.kind=='button':
                        flush_motion()
                        if event.code in (4,5,6,7):
                            if event.down:
                                h=(120 if event.code==7 else -120 if event.code==6 else 0)
                                v=(120 if event.code==5 else -120 if event.code==4 else 0)
                                h,v=self.profile.scroll(h,v);self._push('wheel',dict(horizontal=round(h),vertical=round(v)))
                            continue
                        button=_BUTTONS.get(event.code)
                        if button is None: raise RuntimeError('unsupported button')
                        bit=1<<(button-1)
                        if bool(self.buttons&bit)==event.down: raise RuntimeError('inconsistent button')
                        self.buttons=(self.buttons|bit) if event.down else (self.buttons&~bit)
                        x,y=self._coords();self._push('button',dict(screen_id=self.screen_id,x=x,y=y,button=button,down=int(event.down),buttons=self.buttons))
                if pending_motion is not None and time.monotonic()-last_motion>=1/self.profile.forward_hz:flush_motion()
                self.stop.wait(max(.001,min(.01,1/self.profile.forward_hz/2)))
        except BaseException as exc:self.error=exc
        finally:
            if backend:
                try: backend.ungrab()
                except Exception as exc:self.error=self.error or exc
                try: backend.close()
                except Exception as exc:self.error=self.error or exc
            self.started.set()

    def pump(self, session):
        """Send queued events on TLS owner thread, never from X11 capture thread."""
        if self.thread is None or self.stop.is_set(): raise RuntimeError('capture not running')
        self.last_pump=time.monotonic()
        if self.error or not self.thread.is_alive():
            try: session.close()
            finally: self.restore_local()
            raise RuntimeError('capture stopped; session closed') from self.error
        try:
            while True:
                if self.error or not self.thread.is_alive(): raise RuntimeError('capture stopped')
                with self.lock:
                    if not self.queue: break
                    event,fields=self.queue.popleft()
                session.send_input(event,**fields)
                self.last_pump=time.monotonic()
        except BaseException:
            try:self.restore_local()
            finally:session.close()
            raise

    def restore_local(self):
        self.stop.set()
        if self.thread and threading.current_thread() is not self.thread:
            self.thread.join(2)
            if self.thread.is_alive(): raise RuntimeError('capture release timed out')
        with self.lock:self.queue.clear()

    def release_all(self):
        # Local capture does not inject input; remote releases on session close.
        pass
