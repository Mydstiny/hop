"""Focus-bound bridge between a v2 session and an isolated X11 actor.

Call pump from the authenticated session's owner thread after input handling.
No system selection is read until focus and bilateral READY are established.
"""
import queue
import threading
from .clipboard import TEXT,PNG,validate_payload
from .clipboard_actor import ClipboardActor,ClipboardActorError
from .clipboard_assembler import ClipboardAssembler
from .clipboard_initial import InitialSnapshot
from .clipboard_session import ClipboardSender,ClipboardSession
from .clipboard_sync import ClipboardCoordinator
from .png_decoder import BoundedPNGDecoder


class ClipboardRuntime:
    def __init__(self,library, *, actor_factory=ClipboardActor, decoder_factory=BoundedPNGDecoder):
        self.actor=actor_factory();self.epoch=0;self.operations={};self.read_id=None;self.read_version=None;self.entry_version=None
        self.initial=None;self.last_seen=0;self.validating=False;self.validation_results=queue.Queue(maxsize=1);self.closed=False
        try:
            try:self.decoder=decoder_factory()
            except RuntimeError:self.decoder=None
            self.sender=ClipboardSender(png_decoder=(lambda _data:True) if self.decoder else None)
            assembler=ClipboardAssembler(library);coordinator=ClipboardCoordinator(png_decoder=self.decoder)
            self.inner=ClipboardSession(assembler,coordinator,self._current_version,install_idle=self._install_idle)
            self.supported_capabilities=self.inner.supported_capabilities
        except BaseException:
            self.actor.close();raise

    def _current_version(self):
        # The actor compares against the authoritative version under the X
        # server grab before writing. A delayed notification can only cause a
        # conservative discard, so the session thread never needs a version RPC.
        return self.actor.version

    def focus(self,epoch):
        if self.closed:raise RuntimeError('clipboard runtime closed')
        if epoch:
            if not self.barrier_ready():raise RuntimeError('previous clipboard focus has not drained')
            self.entry_version=self.actor.focus(epoch);self.inner.focus(epoch);self.epoch=epoch;self.last_seen=self.entry_version;self.initial=None
        else:
            self.epoch=0
            ident=self.actor.focus(0);self.operations[ident]=('revoke',0)
            if self.initial and self.initial.state=='reading':self.initial.cancel()
            self.initial=None;self.read_id=None;self.inner.focus(0)

    def expect_initial_snapshot(self):self.inner.expect_initial_snapshot(self.entry_version)
    def disable_access(self):self.actor.disable_access()
    def initial_done(self):self.inner.initial_done()
    def initial_processed(self):
        self._drain()
        return self.inner.initial_processed() and not self.validating and not any(v[0]=='install' for v in self.operations.values())
    def control(self,epoch,event,fields):self.inner.control(epoch,event,fields)
    def chunk(self,epoch,fields):self.inner.chunk(epoch,fields)

    def barrier_ready(self):
        self._drain()
        return not self.operations and not self.validating and self.inner.barrier_ready()

    def _request_source(self,session,epoch):
        caps=session.capabilities & session.peer_capabilities
        targets=tuple(([PNG] if caps&4 else [])+([TEXT] if caps&2 else []))
        if not targets:return False
        self.read_version=self.actor.version
        ident=self.actor.request_read(targets);self.read_id=ident;self.operations[ident]=('read',epoch)
        return True

    def _source_none(self,epoch):
        if self.initial and self.initial.state=='reading':self.initial.resolve(epoch,None,None,self.actor.version)

    def _source_value(self,epoch,target,data,version):
        if epoch!=self.epoch:return
        if version!=self.read_version:
            self._source_none(epoch);return
        try:validate_payload(target,data)
        except ValueError:self._source_none(epoch);return
        initial=bool(self.initial and self.initial.state=='reading')
        if target==PNG:
            if self.decoder is None:self._source_none(epoch);return
            self.validating=True
            def validate():
                try:valid=self.decoder(data) is True
                except Exception:valid=False
                self.validation_results.put((epoch,target,data,version,valid,initial))
            threading.Thread(target=validate,name='hop-clipboard-source-png',daemon=True).start()
        else:self._source_validated(epoch,target,data,version,True,initial)

    def _source_validated(self,epoch,target,data,version,valid,initial):
        if epoch!=self.epoch:return
        if initial and (not self.initial or self.initial.state!='reading'):return
        if not valid:self._source_none(epoch);return
        if initial:self.initial.resolve(epoch,target,data,version)
        elif not self.sender.active:self.sender.start(epoch,target,data,version)

    def _drain(self):
        self.actor.check_health()
        for ident,operation in tuple(self.operations.items()):
            kind,epoch=operation[:2]
            reply=self.actor.take(ident)
            if reply is None:continue
            del self.operations[ident]
            if kind=='install' and reply.get('applied') is True:
                installed=reply.get('installed_version')
                if installed is not None and reply.get('version')==installed:
                    self.inner.rebase_deferred(epoch,operation[2],installed)
            if kind=='read' and not reply.get('started') and self.read_id==ident:
                self.read_id=None
                if epoch==self.epoch:self._source_none(epoch)
        while True:
            try:event=self.actor.events.get_nowait()
            except queue.Empty:break
            if event.get('id')!=self.read_id or event.get('epoch')!=self.epoch:continue
            epoch=self.epoch;self.read_id=None
            if event.get('type')=='read_empty':self._source_none(epoch);continue
            if event.get('type')!='read' or event.get('target') not in (TEXT,PNG) or not isinstance(event.get('data'),bytes):
                raise ClipboardActorError('invalid clipboard source result')
            self._source_value(epoch,event['target'],event['data'],event.get('version'))
        try:result=self.validation_results.get_nowait()
        except queue.Empty:pass
        else:
            self.validating=False;self._source_validated(*result)

    def _install(self,epoch,target,data,version):
        if epoch!=self.epoch:return False
        ident=self.actor.install(epoch,target,data,version);self.operations[ident]=('install',epoch,version)
        return True

    def _install_idle(self):
        return not any(operation[0]=='install' for operation in self.operations.values())

    def pump(self,session):
        if self.closed:raise RuntimeError('clipboard runtime closed')
        self._drain()
        if not self.epoch or session.phase!='remote' or session.epoch!=self.epoch:return
        if not (session.capabilities & session.peer_capabilities & 6):return
        self.inner.poll_apply(self._install)
        if not session._clip_local_ready:session.clipboard_ready()
        if not session._clip_peer_ready:return
        if session.role=='host' and not session._clip_initial_sent:
            if self.initial is None:
                self.initial=InitialSnapshot(self.sender,cancel_read=self.actor.cancel_read)
                self.last_seen=self.actor.version
                self.initial.begin(session,self.last_seen,lambda epoch:self._request_source(session,epoch))
            self.initial.pump(session,self.actor.version)
            if self.initial.state=='done':self.read_id=None
            return
        if not session.clipboard_send_ready():return
        if self.sender.active:
            frame=self.sender.next_frame(self.epoch,self.actor.version)
            if frame:session.send_clipboard_frame(*frame)
        elif self.read_id is None and not self.validating and self.actor.version!=self.last_seen:
            self.last_seen=self.actor.version;self._request_source(session,self.epoch)

    def close(self):
        if self.closed:return
        self.closed=True;self.epoch=0
        try:self.actor.close()
        finally:
            try:self.inner.close()
            finally:self.sender.close()
