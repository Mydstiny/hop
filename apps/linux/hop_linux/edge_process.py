"""Bounded IPC to an explicitly enabled read-only edge observer."""
import json
import queue
import subprocess
import sys
import threading


class EdgeProcess:
    def __init__(self, expected_screens):
        self.expected=tuple(tuple(s) for s in expected_screens)
        self.process=None;self.reader=None;self.ready=threading.Event();self.error=None
        self.events=queue.Queue(maxsize=8)

    def start(self):
        if self.process is not None:raise RuntimeError('edge observer already started')
        self.process=subprocess.Popen([sys.executable,'-B','-m','hop_linux.edge_worker'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,bufsize=0)
        self.reader=threading.Thread(target=self._read,daemon=True,name='hop-edge-ipc');self.reader.start()
        if not self.ready.wait(3) or self.error:
            self.close();raise RuntimeError('edge observer unavailable')

    def _read(self):
        try:
            raw=self.process.stdout.readline(4097)
            if len(raw)>4096 or not raw.endswith(b'\n'):raise RuntimeError('invalid observer greeting')
            hello=json.loads(raw)
            if hello.get('type')!='ready' or tuple(tuple(s) for s in hello.get('screens',()))!=self.expected:
                raise RuntimeError('monitor configuration changed')
            self.ready.set()
            while True:
                raw=self.process.stdout.readline(4097)
                if not raw or len(raw)>4096 or not raw.endswith(b'\n'):raise RuntimeError('edge observer exited')
                event=json.loads(raw)
                if (set(event)!={'type','screen_id','direction','x','y'} or event['type']!='edge' or
                    type(event['screen_id']) is not int or event['screen_id'] not in {s[0] for s in self.expected} or
                    event['direction'] not in ('left','right','up','down') or
                    any(type(event[k]) is not int or not 0<=event[k]<=65535 for k in ('x','y'))):
                    raise RuntimeError('invalid edge event')
                self.events.put_nowait(event)
        except BaseException as exc:
            self.error=exc;self.ready.set()

    def poll(self):
        if self.error or self.process is None or self.process.poll() is not None:raise RuntimeError('edge observer stopped')
        try:return self.events.get_nowait()
        except queue.Empty:return None

    def close(self):
        process=self.process;self.process=None
        if process:
            if process.stdin:
                try:process.stdin.close()
                except OSError:pass
            try:process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:process.wait(timeout=1)
                except subprocess.TimeoutExpired:process.kill();process.wait(timeout=1)
            if process.stdout:process.stdout.close()
