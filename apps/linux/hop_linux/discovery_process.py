"""Explicit lifecycle for the isolated Avahi discovery worker."""
import json
import queue
import subprocess
import sys
import threading
from .discovery import DiscoveryError,validate_hint

class DiscoveryProcess:
    def __init__(self):self.process=None;self.events=queue.Queue(maxsize=16);self.reader=None
    def start(self, *, name, port=None, explicitly_started):
        if explicitly_started is not True or self.process is not None:raise DiscoveryError('explicit discovery start required')
        self.process=subprocess.Popen([sys.executable,'-B','-m','hop_linux.discovery_worker'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
        try:
            self.process.stdin.write((json.dumps({'name':name,'port':port})+'\n').encode());self.process.stdin.flush()
            self.reader=threading.Thread(target=self._read,daemon=True,name='hop-discovery-ipc');self.reader.start()
        except BaseException:self.close();raise
    def _read(self):
        try:
            while self.process:
                raw=self.process.stdout.readline(65537)
                if not raw:break
                if len(raw)>65536 or not raw.endswith(b'\n'):raise DiscoveryError('invalid discovery message')
                item=json.loads(raw)
                if item.get('type')=='hints':
                    values=item.get('items')
                    if not isinstance(values,list) or len(values)>64:raise DiscoveryError('invalid discovery list')
                    item={'type':'hints','items':[validate_hint(v['name'],v['address'],v['port'],v['interface'],[b'v=2']) for v in values]}
                elif item.get('type') not in ('ready','error'):raise DiscoveryError('invalid discovery status')
                self.events.put_nowait(item)
        except Exception:
            try:self.events.put_nowait({'type':'error','message':'Discovery worker failed'})
            except queue.Full:pass
    def poll(self):
        try:return self.events.get_nowait()
        except queue.Empty:return None
    def close(self):
        process=self.process;self.process=None
        if process:
            try:
                if process.poll() is None:
                    try:process.stdin.write(b'stop\n');process.stdin.flush()
                    except (OSError,ValueError):pass
                    try:process.wait(timeout=2)
                    except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=2)
            finally:
                for stream in (process.stdin,process.stdout):
                    try:stream.close()
                    except OSError:pass
                if self.reader and self.reader is not threading.current_thread():self.reader.join(.5)
