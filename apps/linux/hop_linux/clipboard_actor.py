"""Bounded private-process X11 clipboard actor; no import-time desktop access."""
import base64
import json
import mmap
import os
import queue
import socket
import subprocess
import sys
import threading
import time
from .clipboard import TEXT,PNG

_MAX_WIRE=24*1024*1024


class ClipboardActorError(RuntimeError):pass


class ClipboardActor:
    def __init__(self):
        if not hasattr(os,'memfd_create'):raise ClipboardActorError('private memory descriptor unavailable')
        self.fd=os.memfd_create('hop-clipboard-gate',getattr(os,'MFD_CLOEXEC',0));os.ftruncate(self.fd,1)
        self.gate=mmap.mmap(self.fd,1);self.gate[0]=0
        self.disabled=False
        parent,child=socket.socketpair();self.socket=parent;self.socket.settimeout(2)
        self.process=None;self.error=None;self.ready=threading.Event();self.condition=threading.Condition()
        self.requests=queue.Queue(maxsize=16);self.events=queue.Queue(maxsize=8);self.responses={};self.ignored=set();self.next_id=1;self.version=0;self.epoch=0
        try:
            self.process=subprocess.Popen([sys.executable,'-B','-m','hop_linux.clipboard_actor_worker',str(child.fileno()),str(self.fd)],
                pass_fds=(child.fileno(),self.fd),stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        except BaseException:
            parent.close();child.close();self.gate.close();os.close(self.fd);raise
        child.close()
        self.reader=threading.Thread(target=self._read,name='hop-clipboard-read',daemon=True)
        self.writer=threading.Thread(target=self._write,name='hop-clipboard-write',daemon=True)
        self.reader.start();self.writer.start()
        if not self.ready.wait(3) or self.error:
            self.close();raise ClipboardActorError('clipboard actor unavailable')

    def _fail(self,exc):
        with self.condition:
            if self.gate is not None:self.gate[0]=0
            if self.error is None:self.error=exc
            self.condition.notify_all()
        self.ready.set()

    def _observe_version(self,version):
        if type(version) is not int or version<self.version:raise ClipboardActorError('invalid clipboard owner version')
        self.version=version

    def _read(self):
        buffer=bytearray()
        try:
            while True:
                try:chunk=self.socket.recv(65536)
                except socket.timeout:continue
                if not chunk:raise ClipboardActorError('clipboard actor exited')
                parts=chunk.split(b'\n');buffer.extend(parts[0])
                if len(buffer)>_MAX_WIRE:raise ClipboardActorError('clipboard IPC response too large')
                for tail in parts[1:]:
                    line=buffer;buffer=bytearray(tail)
                    item=json.loads(line)
                    if not isinstance(item,dict):raise ClipboardActorError('invalid clipboard response')
                    if item.get('type')=='ready':
                        self._observe_version(item['version']);self.ready.set();continue
                    if item.get('type')=='version':self._observe_version(item['version']);continue
                    if item.get('type') in ('read','read_empty'):
                        if item['type']=='read':
                            if item.get('target') not in (TEXT,PNG):raise ClipboardActorError('invalid clipboard source format')
                            item['data']=base64.b64decode(item['data'],validate=True)
                            self._observe_version(item['version'])
                        try:self.events.put_nowait(item)
                        except queue.Full:raise ClipboardActorError('clipboard events overflow')
                        continue
                    ident=item.get('id')
                    if type(ident) is not int or item.get('ok') is not True:raise ClipboardActorError('invalid clipboard acknowledgment')
                    with self.condition:
                        if 'version' in item:self._observe_version(item['version'])
                        if ident in self.ignored:self.ignored.remove(ident);continue
                        if len(self.responses)>=32 or ident in self.responses:raise ClipboardActorError('clipboard acknowledgments overflow')
                        self.responses[ident]=item;self.condition.notify_all()
        except BaseException as exc:self._fail(exc)

    def _write(self):
        try:
            while True:
                item=self.requests.get()
                if item is None:return
                if item['op']=='install':item['data']=base64.b64encode(item['data']).decode('ascii')
                data=json.dumps(item,separators=(',',':')).encode()+b'\n'
                if len(data)>_MAX_WIRE:raise ClipboardActorError('clipboard IPC command too large')
                self.socket.sendall(data)
        except BaseException as exc:self._fail(exc)

    def check_health(self):
        if self.error or not self.process or self.process.poll() is not None:
            raise ClipboardActorError('clipboard actor unavailable') from self.error

    def send(self,op,**fields):
        self.check_health()
        ident=self.next_id;self.next_id+=1
        try:self.requests.put_nowait({'id':ident,'op':op,**fields})
        except queue.Full:raise ClipboardActorError('clipboard command queue full')
        return ident

    def take(self,ident):
        with self.condition:
            if self.error:raise ClipboardActorError('clipboard actor failed') from self.error
            return self.responses.pop(ident,None)

    def call(self,op,timeout=1,**fields):
        ident=self.send(op,**fields);deadline=time.monotonic()+timeout
        with self.condition:
            while ident not in self.responses and not self.error:
                remaining=deadline-time.monotonic()
                if remaining<=0:raise ClipboardActorError('clipboard actor timed out')
                self.condition.wait(remaining)
            if self.error:raise ClipboardActorError('clipboard actor failed') from self.error
            return self.responses.pop(ident)

    def focus(self,epoch):
        if type(epoch) is not int or not 0<=epoch<2**64:raise ValueError('invalid clipboard focus')
        self.gate[0]=0
        self.epoch=0
        if epoch:
            reply=self.call('focus',epoch=epoch)
            with self.condition:
                self.check_health();self.epoch=epoch
                if not self.disabled:self.gate[0]=1
            return reply['version']
        return self.send('focus',epoch=0)

    def disable_access(self):
        with self.condition:
            self.disabled=True
            if self.gate is not None:self.gate[0]=0

    def request_read(self,targets):
        if not self.epoch:raise ClipboardActorError('clipboard focus inactive')
        return self.send('read',targets=list(targets))
    def cancel_read(self):
        ident=self.send('cancel')
        with self.condition:
            if ident in self.responses:self.responses.pop(ident)
            else:self.ignored.add(ident)
    def install(self,epoch,target,data,version):
        if epoch!=self.epoch or not epoch:raise ClipboardActorError('clipboard focus inactive')
        if target not in (TEXT,PNG) or not isinstance(data,bytes):raise ValueError('invalid clipboard value')
        return self.send('install',epoch=epoch,target=target,version=version,data=data)

    def close(self):
        if self.gate is None:return
        self.gate[0]=0
        try:self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:pass
        self.socket.close()
        try:self.requests.put_nowait(None)
        except queue.Full:pass
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=1)
        for thread in (self.reader,self.writer):
            if thread is not threading.current_thread():thread.join(.5)
        with self.condition:
            self.gate.close();self.gate=None
        os.close(self.fd)
