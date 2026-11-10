"""Single-owner coordinator for several already authenticated v2 sessions.

No discovery, UI, capture or connections are started here. The owner must call
tick on one thread and supply a fresh capture sink for each ENTER. A switch is
serialized through the old session's RELEASE_ACK and both clipboard barriers.
"""
import time


class MultiSessionError(RuntimeError):pass


class MultiSessionHost:
    def __init__(self, sessions, capture_factory, input_state_clear, *, clock=time.monotonic):
        if not sessions or len(sessions)>64 or not callable(capture_factory) or not callable(input_state_clear):raise ValueError('sessions and capture callbacks required')
        self.sessions=dict(sessions);self.capture_factory=capture_factory;self.input_state_clear=input_state_clear;self.clock=clock
        for peer,session in self.sessions.items():
            if (not isinstance(peer,str) or not peer or getattr(session,'role',None)!='host' or
                not getattr(session.transport,'authenticated',False) or
                getattr(session.transport,'peer_fingerprint',None)!=peer or session.phase!='local'):
                raise ValueError('authenticated idle host sessions required')
        self.active=None;self.pending=None;self.phase='local';self.capture=None;self.deadline=None;self.closed=False
        self._local_return=None

    def switch(self, peer, screen_id, x, y):
        if self.closed or self.pending is not None or self.phase not in ('local','remote'):raise MultiSessionError('switch unavailable')
        if peer not in self.sessions:raise MultiSessionError('invalid target')
        session=self.sessions[peer]
        expected_phase='remote' if peer==self.active else 'local'
        if (session.closed or session.phase!=expected_phase or screen_id not in {s[0] for s in session.screens} or
            (peer==self.active and screen_id==session.screen_id)):
            raise MultiSessionError('target screen unavailable')
        if any(type(v) is not int or not 0<=v<=65535 for v in (x,y)):raise ValueError('invalid coordinates')
        if self.input_state_clear() is not True:raise MultiSessionError('physical inputs are held')
        self.pending=(peer,screen_id,x,y);self.deadline=self.clock()+6
        try:
            if self.active is not None:
                self.phase='releasing';self.sessions[self.active].release();self.capture=None
            else:self._enter_pending()
        except BaseException:self.close();raise

    def _enter_pending(self):
        if self.input_state_clear() is not True:raise MultiSessionError('physical inputs changed during handoff')
        peer,screen,x,y=self.pending;session=self.sessions[peer]
        if session.closed or session.phase!='local' or not session.transport.authenticated:raise MultiSessionError('target disconnected')
        capture=self.capture_factory(peer,screen,x,y)
        if capture is None:raise MultiSessionError('capture unavailable')
        self.capture=capture;session.sink=capture;self.phase='entering';session.enter(screen,x,y)

    def return_local(self, callback):
        if self.closed or self.phase!='remote' or self.active is None or self.pending is not None or not callable(callback):
            raise MultiSessionError('local return unavailable')
        if self.input_state_clear() is not True:raise MultiSessionError('physical inputs are held')
        self._local_return=callback;self.pending=('local',);self.deadline=self.clock()+6;self.phase='releasing'
        try:self.sessions[self.active].release();self.capture=None
        except BaseException:self.close();raise

    def route_edge(self,layout,local_id,source_id,screen_id,direction,x,y,return_local):
        """Route a validated edge; caller must verify current physical position."""
        from .layout_router import route_edge
        if (self.phase=='local' and source_id!=local_id) or (self.phase=='remote' and source_id!=self.active):
            raise MultiSessionError('edge source is not active')
        if self.phase not in ('local','remote'):raise MultiSessionError('switch pending')
        route=route_edge(layout,source_id,str(screen_id),direction,x,y)
        if not route:return False
        if route.device_id==local_id:
            if self.phase!='remote':return False
            if not callable(return_local):raise ValueError('local pointer restoration callback required')
            self.return_local(lambda:return_local(route));return True
        session=self.sessions.get(route.device_id)
        if session is None:return False
        target=next((s[0] for s in session.screens if str(s[0])==route.screen_id),None)
        if target is None:return False
        self.switch(route.device_id,target,route.x,route.y);return True

    def tick(self,timeout=.001):
        if self.closed:raise MultiSessionError('coordinator closed')
        if not isinstance(timeout,(int,float)) or not 0<timeout<=.05:raise ValueError('invalid poll timeout')
        try:
            if self.deadline is not None and self.clock()>=self.deadline:raise MultiSessionError('switch timed out')
            for session in self.sessions.values():
                if session.closed or not session.transport.authenticated:raise MultiSessionError('session disconnected')
                session.poll(timeout)
            if self.phase=='releasing' and self.sessions[self.active].phase=='local':
                self.active=None
                if self._local_return:
                    if self.input_state_clear() is not True:raise MultiSessionError('physical inputs changed during handoff')
                    callback=self._local_return;self._local_return=None;self.pending=None;self.phase='local';self.deadline=None;callback()
                else:self._enter_pending()
            if self.phase=='entering':
                peer,_,_,_=self.pending;session=self.sessions[peer]
                if session.phase=='remote':
                    self.capture.start(session);self.active=peer;self.pending=None;self.phase='remote';self.deadline=None
            if self.phase=='remote':self.capture.pump(self.sessions[self.active])
        except BaseException:self.close();raise

    def close(self):
        if self.closed:return
        self.closed=True;self.phase='closed';failures=[]
        if self.capture:
            try:self.capture.restore_local()
            except Exception as exc:failures.append(exc)
            self.capture=None
        for session in self.sessions.values():
            try:session.close()
            except Exception as exc:failures.append(exc)
        if failures:raise MultiSessionError('session cleanup incomplete') from failures[0]
