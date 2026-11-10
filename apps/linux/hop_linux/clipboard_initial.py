"""Nonblocking initial host snapshot driver; caller owns the system actor.

The actor must request only supported text/PNG and refuse file objects. It
reports the owner version associated with the completed read; this driver
emits at most one frame per pump and places INITIAL_DONE after every chunk.
"""
import time
from . import codec


class InitialSnapshot:
    def __init__(self,sender, *, cancel_read, clock=time.monotonic, read_timeout=10):
        if not callable(cancel_read):raise ValueError('read cancellation required')
        if not callable(clock) or not isinstance(read_timeout,(int,float)) or not 0<read_timeout<=10:raise ValueError('invalid read timeout')
        self.sender=sender;self.epoch=0;self.source_version=None;self.state='idle'
        self.cancel_read=cancel_read;self.clock=clock;self.read_timeout=read_timeout;self.deadline=None

    def begin(self,session,source_version,request_read):
        if (self.state!='idle' or session.role!='host' or not session.clipboard_send_ready() or
            session._clip_initial_sent or self.sender.epoch!=session.epoch or self.sender.active is not None or
            type(source_version) is not int or source_version<0 or not callable(request_read)):
            raise RuntimeError('initial clipboard read unavailable')
        self.epoch=session.epoch;self.source_version=source_version;self.state='reading';self.deadline=self.clock()+self.read_timeout
        try:
            if request_read(self.epoch) is False:self.state='skip'
        except BaseException:self.state='skip';raise

    def resolve(self,epoch,target,data,source_version):
        """Called after the actor completes; stale focus/owner data is discarded."""
        if self.state!='reading' or epoch!=self.epoch:return False
        if source_version!=self.source_version or target is None:
            self.state='skip';return False
        try:self.sender.start(epoch,target,data,source_version)
        except (TypeError,ValueError,PermissionError,RuntimeError):self.state='skip';return False
        self.state='sending';return True

    def pump(self,session,current_version):
        if self.state in ('idle','done','canceled'):return False
        if session.closed or session.phase!='remote' or session.epoch!=self.epoch:
            self.cancel();return False
        if not session.clipboard_send_ready():return False
        if self.state=='reading':
            if self.clock()<self.deadline:return False
            self.cancel_read()
            self.state='skip'
        if self.state=='sending':
            frame=self.sender.next_frame(self.epoch,current_version)
            if frame is not None:
                session.send_clipboard_frame(*frame);return True
        session.send_clipboard_frame(3,codec.encode_clipboard_control('initial_done'))
        self.state='done';return True

    def cancel(self):
        if self.state=='reading':self.cancel_read()
        self.sender.focus(0);self.epoch=0;self.source_version=None;self.state='canceled'
