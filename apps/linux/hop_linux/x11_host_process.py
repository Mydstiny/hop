"""Explicit isolated X11 capture controller, suitable as a Session host sink.

The parent may already have initialized GTK; XInitThreads runs in a fresh child
before any Xlib use. XI2 button state is checked after grabbing; it is the
server's logical state, and cannot eliminate every hardware timing race.
Shortcut translation, multi-monitor routing and packaging remain open.
"""
from dataclasses import asdict
import json
import os
import queue
import subprocess
import sys
import threading
import time
from .profiles import PointerProfile


class X11HostProcess:
    def __init__(self, *, screen_id, target_size, capture_center, initial=(32768,32768), profile=None, translator=None, edge_directions=(), on_edge=None):
        self.config=dict(screen_id=screen_id,target_size=target_size,capture_center=capture_center,initial=initial,
                         profile=asdict(profile or PointerProfile()),edge_directions=edge_directions)
        self.process=None;self.session=None;self.reader=None;self.error=None;self.started=threading.Event()
        self.translator=translator
        self.on_edge=on_edge
        self.events=queue.Queue(maxsize=256);self.last_tick=0

    def start(self, session):
        if (self.process is not None or getattr(session,'sink',None) is not self or
            getattr(session,'role',None)!='host' or getattr(session,'phase',None)!='remote' or
            not getattr(getattr(session,'transport',None),'authenticated',False)):
            raise RuntimeError('authenticated acknowledged session required')
        self.session=session
        self.process=subprocess.Popen([sys.executable,'-B','-m','hop_linux.capture_worker'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,bufsize=0)
        try:
            self.process.stdin.write((json.dumps(self.config)+'\n').encode())
            self.reader=threading.Thread(target=self._read,daemon=True,name='hop-capture-ipc');self.reader.start()
            if not self.started.wait(3) or self.error: raise RuntimeError('capture subprocess failed to start')
            os.set_blocking(self.process.stdin.fileno(),False)
            self._tick()
        except BaseException:
            try:self.restore_local()
            finally:session.close()
            raise

    def _read(self):
        try:
            while True:
                raw=self.process.stdout.readline(16385)
                if not raw: raise RuntimeError('capture subprocess exited')
                if len(raw)>16384 or not raw.endswith(b'\n'): raise RuntimeError('invalid capture message')
                message=json.loads(raw)
                if message.get('type')=='ready': self.started.set();continue
                if message.get('type')=='error': raise RuntimeError('capture failed: '+str(message.get('message','')))
                if message.get('type')!='input' or not isinstance(message.get('fields'),dict) or not isinstance(message.get('event'),str): raise RuntimeError('invalid capture message')
                try:self.events.put_nowait((message['event'],message['fields']))
                except queue.Full: raise RuntimeError('capture IPC queue full')
        except BaseException as exc:
            self.error=exc;self.started.set()

    def _tick(self):
        if self.process is None or self.process.poll() is not None or self.error: raise RuntimeError('capture subprocess stopped')
        if time.monotonic()-self.last_tick>=.2:
            if os.write(self.process.stdin.fileno(),b'tick\n')!=5: raise RuntimeError('capture heartbeat failed')
            self.last_tick=time.monotonic()

    def pump(self, session):
        try:
            if session is not self.session or session.phase!='remote' or not session.transport.authenticated: raise RuntimeError('active authenticated focus required')
            self._tick()
            deadline=time.monotonic()+.05
            for _ in range(32):
                try:event,fields=self.events.get_nowait()
                except queue.Empty:break
                if self.error: raise RuntimeError('capture subprocess failed')
                if event=='edge':
                    if self.on_edge and self.on_edge(session,fields):return
                    continue
                if self.translator:
                    translated=self.translator.push(**fields) if event=='key' else self.translator.flush_pointer()
                    for key in translated: session.send_input('key',**key)
                    if event!='key': session.send_input(event,**fields)
                else: session.send_input(event,**fields)
                self._tick()
                if time.monotonic() >= deadline: break
        except BaseException:
            try:self.restore_local()
            finally:session.close()
            raise

    def restore_local(self):
        child=self.process
        if child is None:
            if self.translator:
                translator,self.translator=self.translator,None
                try:translator.release()
                finally:translator.close()
            return
        self.process=None
        try:
            if child.poll() is None:
                try: os.write(child.stdin.fileno(),b'stop\n')
                except (OSError,ValueError):pass
                try: child.wait(timeout=1.5)
                except subprocess.TimeoutExpired:
                    child.terminate()
                    try:child.wait(timeout=.5)
                    except subprocess.TimeoutExpired:child.kill();child.wait(timeout=1)
        finally:
            for stream in (child.stdin,child.stdout):
                try:stream.close()
                except OSError:pass
            if self.reader and self.reader is not threading.current_thread():self.reader.join(.5)
            while not self.events.empty():
                try:self.events.get_nowait()
                except queue.Empty:break
            # The receiver releases every injected key upon RELEASE/disconnect.
            # Translator release here clears its own state; never send from a
            # cleanup callback after focus has been revoked.
            if self.translator:
                translator,self.translator=self.translator,None
                try:translator.release()
                finally:translator.close()

    def release_all(self):pass
