"""Opt-in v2 clipboard receive boundary; does not access real selections.

Caller provides a monotonic local-owner version and schedules poll_apply on the
clipboard owner thread. Cross-process selection change tracking is not wired.
"""
from .clipboard import TEXT,PNG
from .clipboard_assembler import ClipboardAssemblyError

class ClipboardSession:
    def __init__(self,assembler,coordinator,current_version, *, install_idle=lambda:True):
        if not callable(current_version):raise ValueError('local clipboard version callback required')
        self.assembler,self.coordinator,self.current_version=assembler,coordinator,current_version
        self.install_idle=install_idle
        self.supported_capabilities=2 | (4 if callable(coordinator.png_decoder) else 0)
        self.epoch=0;self.active_id=None;self.version=None;self.initial_version=None;self.deferred=None

    def focus(self,epoch):
        if self.deferred:
            data=self.deferred[2];data[:]=b'\0'*len(data);self.deferred=None
        self.epoch=epoch;self.active_id=None;self.version=None;self.initial_version=None
        self.assembler.focus(epoch);self.coordinator.focus(epoch)

    def expect_initial_snapshot(self,version=None):
        """Capture target version at focus entry, before either side is ready."""
        if not self.epoch or self.active_id is not None or self.initial_version is not None:
            raise ClipboardAssemblyError('initial clipboard snapshot unavailable')
        if version is None:version=self.current_version()
        if type(version) is not int or version<0:raise ClipboardAssemblyError('invalid local clipboard version')
        self.initial_version=version

    def finish_initial_without_transfer(self):
        if not self.epoch or self.active_id is not None:raise ClipboardAssemblyError('initial snapshot still active')
        self.initial_version=None

    def initial_done(self):
        if self.active_id is not None:raise ClipboardAssemblyError('initial snapshot is incomplete')
        if self.initial_version is not None:self.finish_initial_without_transfer()

    def control(self,epoch,event,fields):
        if not epoch or epoch!=self.epoch:raise ClipboardAssemblyError('inactive clipboard focus')
        if event=='start':
            if self.active_id is not None:raise ClipboardAssemblyError('clipboard transfer already active')
            self.version=self.initial_version if self.initial_version is not None else self.current_version()
            if type(self.version) is not int or self.version<0:raise ClipboardAssemblyError('invalid local clipboard version')
            self.assembler.begin(epoch,fields['transfer_id'],fields['format'],fields['total']);self.active_id=fields['transfer_id'];self.initial_version=None
            self._finish_if_ready()
        elif event=='cancel':
            if self.active_id!=fields['transfer_id']:raise ClipboardAssemblyError('unknown clipboard cancellation')
            self.assembler.cancel();self.active_id=None;self.version=None
        else:raise ClipboardAssemblyError('unsupported clipboard control')

    def chunk(self,epoch,fields):
        if epoch!=self.epoch or fields['transfer_id']!=self.active_id:raise ClipboardAssemblyError('inactive clipboard transfer')
        self.assembler.chunk(epoch,fields['transfer_id'],fields['offset'],fields['data'])
        self._finish_if_ready()

    def _finish_if_ready(self):
        result=self.assembler.take()
        if result is None:return
        fmt,epoch,_ident,output=result;target=TEXT if fmt==1 else PNG
        try:
            if self.deferred or self.coordinator.busy() or not self.install_idle():
                if self.deferred:
                    old=self.deferred[2];old[:]=b'\0'*len(old)
                self.deferred=(epoch,target,bytearray(output),self.version)
            else:self.coordinator.submit(epoch,target,bytes(output),self.version)
        finally:
            if output:output[:]=b'\0'*len(output)
            self.active_id=None;self.version=None

    def poll_apply(self,apply):
        if not self.install_idle():return 'empty'
        result=self.coordinator.poll(self.current_version,apply)
        if self.deferred and not self.coordinator.busy() and self.install_idle():
            epoch,target,data,version=self.deferred;self.deferred=None
            try:
                if epoch==self.epoch:self.coordinator.submit(epoch,target,bytes(data),version)
            finally:data[:]=b'\0'*len(data)
        return result

    def rebase_deferred(self,epoch,old_version,new_version):
        if epoch!=self.epoch or type(new_version) is not int or new_version<old_version:return
        if self.active_id is not None and self.version==old_version:self.version=new_version
        if self.deferred and self.deferred[3]==old_version:
            self.deferred=(*self.deferred[:3],new_version)

    def barrier_ready(self):return self.coordinator.barrier_ready()

    def initial_processed(self):
        return self.active_id is None and self.deferred is None and self.coordinator.quiescent()

    def close(self):
        try:self.focus(0)
        finally:
            try:self.assembler.close()
            finally:self.coordinator.close()


class ClipboardSender:
    """One active focus-gated outbound clipboard value, emitted one frame at a time.

    Preparation and optional PNG decoding belong on a worker. The application
    must schedule interactive input before calling next_frame for another chunk.
    """
    def __init__(self, *, png_decoder=None):
        self.epoch=0;self.next_id=1;self.active=None;self.png_decoder=png_decoder

    def focus(self,epoch):
        if type(epoch) is not int or epoch<0:raise ValueError('invalid focus epoch')
        self._wipe();self.epoch=epoch

    def _wipe(self):
        if self.active:
            data=self.active['data']
            if data:data[:]=b'\0'*len(data)
        self.active=None

    def start(self,epoch,target,data,local_version):
        from .clipboard import validate_payload
        if not self.epoch or epoch!=self.epoch:raise PermissionError('clipboard peer is not active')
        if self.active:raise RuntimeError('clipboard sender busy')
        if self.next_id>=2**64 or type(local_version) is not int or local_version<0:raise ValueError('invalid transfer state')
        validate_payload(target,data)
        if target==PNG and (not callable(self.png_decoder) or self.png_decoder(data) is not True):raise ValueError('PNG decoder rejected content')
        ident=self.next_id;self.next_id+=1
        self.active={'epoch':epoch,'id':ident,'target':target,'data':bytearray(data),'version':local_version,'offset':0,'started':False}
        return ident

    def next_frame(self,epoch,current_version):
        from . import codec
        entry=self.active
        if not entry:return None
        if epoch!=self.epoch or epoch!=entry['epoch']:
            self._wipe();return None
        if type(current_version) is not int or current_version<0:raise ValueError('invalid clipboard version')
        if current_version!=entry['version']:
            payload=codec.encode_clipboard_control('cancel',transfer_id=entry['id'],reason=0) if entry['started'] else None
            self._wipe();return (3,payload) if payload else None
        if not entry['started']:
            entry['started']=True
            payload=codec.encode_clipboard_control('start',transfer_id=entry['id'],format=1 if entry['target']==TEXT else 2,total=len(entry['data']))
            if not entry['data']:self._wipe()
            return 3,payload
        offset=entry['offset'];part=bytes(entry['data'][offset:offset+65520]);entry['offset']+=len(part)
        payload=codec.encode_clipboard_chunk(entry['id'],offset,part)
        if entry['offset']==len(entry['data']):self._wipe()
        return 4,payload

    def close(self):self._wipe();self.epoch=0
