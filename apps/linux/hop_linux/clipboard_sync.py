"""Off-thread clipboard validation with focus and local-version conflict gates.

The caller owns the X11 selection thread/process and a monotonic local clipboard
version incremented for every local owner/content change. No system clipboard
is read or written by this module. PNG needs an independently bounded decoder;
without one it is explicitly rejected rather than advertised as supported.
"""
from dataclasses import dataclass
import queue
import threading
from .clipboard import TEXT,PNG,MAX_TEXT,MAX_PNG,validate_payload


@dataclass(frozen=True)
class ClipboardResult:
    epoch:int
    target:str
    data:bytes
    local_version:int
    generation:int
    error:Exception | None = None


class ClipboardCoordinator:
    def __init__(self, *, png_decoder=None):
        self.png_decoder=png_decoder;self.epoch=0;self.generation=0;self.closed=False
        self.jobs=queue.Queue(maxsize=1);self.results=queue.Queue(maxsize=1);self.pending=False
        self.lock=threading.Lock();self.working=False;self.applying=0
        self.stop=threading.Event()
        self.worker=threading.Thread(target=self._run,name='hop-clipboard-validator',daemon=True);self.worker.start()

    def focus(self,epoch):
        if type(epoch) is not int or epoch<0:raise ValueError('invalid focus epoch')
        with self.lock:self.epoch=epoch;self.generation+=1

    def submit(self,epoch,target,data,local_version):
        if target not in (TEXT,PNG) or not isinstance(data,bytes) or type(local_version) is not int or local_version<0:
            raise ValueError('invalid clipboard value')
        if len(data)>(MAX_TEXT if target==TEXT else MAX_PNG):raise ValueError('clipboard value too large')
        if target==PNG and not callable(self.png_decoder):raise ValueError('bounded PNG decoder unavailable')
        with self.lock:
            if self.closed or not self.epoch or epoch!=self.epoch:raise PermissionError('clipboard target is not active')
            if self.pending:raise RuntimeError('clipboard validation busy')
            self.pending=True;self.working=True
            try:self.jobs.put_nowait((epoch,target,data,local_version,self.generation))
            except queue.Full:self.pending=False;self.working=False;raise RuntimeError('clipboard validation busy')

    def _run(self):
        while True:
            item=self.jobs.get()
            if item is None or self.stop.is_set():return
            epoch,target,data,version,generation=item;error=None
            try:
                validate_payload(target,data)
                if target==PNG and self.png_decoder(data) is not True:raise ValueError('PNG decoder rejected content')
            except Exception as exc:error=exc
            if self.stop.is_set():return
            with self.lock:
                self.results.put_nowait(ClipboardResult(epoch,target,data,version,generation,error))
                self.working=False

    def poll(self,current_version,apply):
        """Run on the clipboard owner thread; return applied/discarded/empty."""
        if not callable(current_version) or not callable(apply):raise ValueError('clipboard callbacks required')
        with self.lock:
            if self.closed:raise RuntimeError('clipboard coordinator closed')
            try:result=self.results.get_nowait()
            except queue.Empty:return 'empty'
            self.pending=False
            if (result.error is not None or not self.epoch or result.epoch!=self.epoch or
                result.generation!=self.generation):return 'discarded'
            self.applying+=1
        # Clipboard ownership and version checks must be serialized by caller;
        # apply must recheck the same epoch in its own X11 event loop.
        try:
            if current_version()!=result.local_version:return 'discarded'
            if apply(result.epoch,result.target,result.data,result.local_version) is False:return 'discarded'
            return 'applied'
        finally:
            with self.lock:self.applying-=1

    def barrier_ready(self):
        """Nonblocking revocation barrier, including running owner callbacks."""
        with self.lock:
            if self.epoch:raise RuntimeError('clipboard focus has not been revoked')
            if self.working or self.applying:return False
            while True:
                try:self.results.get_nowait()
                except queue.Empty:break
            self.pending=False
            return True

    def quiescent(self):
        """Whether every submitted validation and owner callback has settled."""
        with self.lock:return not self.pending and not self.working and not self.applying

    def busy(self):
        with self.lock:return self.pending or self.working

    def close(self):
        if self.closed:return
        with self.lock:self.closed=True;self.epoch=0;self.generation+=1
        self.stop.set()
        # No new job is accepted. Worker may still validate one in-flight value.
        try:self.jobs.put_nowait(None)
        except queue.Full:pass
        self.worker.join(2)
        if self.worker.is_alive():raise RuntimeError('clipboard worker did not stop')
        while True:
            try:self.results.get_nowait()
            except queue.Empty:break
