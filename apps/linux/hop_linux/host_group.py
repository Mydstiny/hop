"""Thread-safe focus ownership for independently owned authenticated sessions.

Each TLS session remains on its creating worker. This coordinator never calls a
session or capture API across threads. A reserved target is identified by the
exact authenticated session object, not by discovery data or screen ID alone.
"""
import threading
from dataclasses import replace
from .layout import Layout,Device


class HostGroupError(RuntimeError):pass


def _same(pair,controller,session):
    return pair is not None and pair[0] is controller and pair[1] is session


class FocusAuthority:
    def __init__(self):
        self._lock=threading.Lock();self._sessions={};self._owner=None;self._pending=None;self._broken=False
        self._handoffs_suspended=False

    def register(self,controller,session):
        peer=session.transport.peer_fingerprint
        if not peer or not session.transport.authenticated:raise HostGroupError('authenticated peer required')
        with self._lock:
            if peer in self._sessions:raise HostGroupError('peer already connected')
            self._sessions[peer]=(controller,session)

    def claim(self,controller,session, *, handoff=False):
        peer=session.transport.peer_fingerprint
        with self._lock:
            if self._broken:raise HostGroupError('focus recovery failed; restart required')
            if handoff and self._handoffs_suspended:raise HostGroupError('edge handoffs are paused')
            if not _same(self._sessions.get(peer),controller,session):raise HostGroupError('session is no longer registered')
            if self._owner is None and self._pending is None:self._owner=(controller,session);return
            if _same(self._owner,controller,session) and self._pending is None:return
            raise HostGroupError('another session owns focus')

    def reserve(self,controller,session,target_peer,route):
        with self._lock:
            if self._broken or self._handoffs_suspended:return False
            target=self._sessions.get(target_peer)
            if not _same(self._owner,controller,session) or self._pending is not None or target is None:return False
            if not target[1].transport.authenticated or target[1].closed:return False
            self._pending=(target[0],target[1],route);return True

    def released(self,controller,session):
        """Called only after local revocation and remote RELEASE_ACK."""
        with self._lock:
            if not _same(self._owner,controller,session):raise HostGroupError('stale focus release')
            pending=self._pending;self._pending=None;self._owner=None
            if pending:
                target,target_session,route=pending;peer=target_session.transport.peer_fingerprint
                if (not _same(self._sessions.get(peer),target,target_session) or target_session.closed or
                    not target_session.transport.authenticated):return None
                self._owner=(target,target_session)
            return pending

    def abort(self,controller,session):
        with self._lock:
            if _same(self._owner,controller,session):self._owner=None;self._pending=None
            elif _same(self._pending,controller,session):self._pending=None

    def unregister(self,controller,session):
        with self._lock:
            for peer,pair in tuple(self._sessions.items()):
                if _same(pair,controller,session):del self._sessions[peer]
            if _same(self._owner,controller,session):self._owner=None;self._pending=None
            elif _same(self._pending,controller,session):self._pending=None

    def idle(self):
        with self._lock:return not self._broken and self._owner is None and self._pending is None

    def suspend_handoffs(self,suspended):
        with self._lock:
            self._handoffs_suspended=suspended
            if suspended:self._pending=None

    def poison(self):
        with self._lock:self._broken=True;self._pending=None

    def while_idle(self,action):
        with self._lock:
            if self._broken or self._owner is not None or self._pending is not None:raise HostGroupError('focus is active')
            return action()


class HostGroup:
    """Combine live, individually authenticated devices into one layout."""
    def __init__(self,layout_store=None):
        self.authority=FocusAuthority();self.layout_store=layout_store;self._lock=threading.Lock()
        self._members={};self.layout=None;self.local=None;self.edges_enabled=False

    def register(self,controller,session):
        self.authority.register(controller,session)
        with self._lock:
            self._members[session.transport.peer_fingerprint]=(controller,session,None)
            controller.edge_enabled=self.edges_enabled
            if self.edges_enabled:controller._event({'type':'edge_enabled'})

    def set_edges_enabled(self,enabled, *, pause_handoffs=False):
        if type(enabled) is not bool:raise ValueError('explicit edge setting required')
        if pause_handoffs or enabled:self.authority.suspend_handoffs(not enabled)
        with self._lock:
            self.edges_enabled=enabled
            for controller,_session,_remote in self._members.values():
                controller.edge_enabled=enabled;controller._event({'type':'edge_enabled' if enabled else 'edge_disabled'})

    @staticmethod
    def _signature(device):return {(s.id,s.width,s.height) for s in device.screens}

    def update_screens(self,controller,session,local,remote):
        if not isinstance(local,Device) or not isinstance(remote,Device):raise HostGroupError('invalid screens')
        peer=session.transport.peer_fingerprint
        if remote.id!=peer:raise HostGroupError('peer identity mismatch')
        with self._lock:
            member=self._members.get(peer)
            if not member or not _same(member,controller,session):raise HostGroupError('stale session screens')
            if self.local and (local.id!=self.local.id or self._signature(local)!=self._signature(self.local)):
                raise HostGroupError('local monitor configuration changed')
            self.local=local;self._members[peer]=(controller,session,remote)
            self._rebuild_locked()

    def _rebuild_locked(self):
        if not self.local:self.layout=None;return
        existing={d.id:d for d in self.layout.devices} if self.layout else {}
        if self.layout_store:
            try:
                saved=self.layout_store.load()
                for device in saved.devices:existing.setdefault(device.id,device)
            except (FileNotFoundError,OSError,ValueError):pass
        def saved_or(device):
            old=existing.get(device.id)
            return old if old and self._signature(old)==self._signature(device) else device
        local=saved_or(self.local);devices=[local]
        for _peer,(_controller,_session,remote) in sorted(self._members.items()):
            if remote is None:continue
            candidate=saved_or(remote)
            if candidate is remote or Layout(tuple(devices+[candidate])).validate().errors:
                right=max(d.bounds()[0]+d.bounds()[2] for d in devices)
                top=min(d.bounds()[1] for d in devices)
                candidate=replace(remote,x=right-min(s.x for s in remote.screens),y=top-min(s.y for s in remote.screens))
            devices.append(candidate)
        layout=Layout(tuple(devices))
        if layout.validate().errors:raise HostGroupError('combined layout overlaps')
        self.layout=layout
        for controller,_session,_remote in self._members.values():
            controller.layout=layout;controller._event({'type':'layout_changed'})

    def set_layout(self,layout):
        if not isinstance(layout,Layout) or layout.validate().errors:raise HostGroupError('layout unavailable')
        def commit():
            with self._lock:
                if not self.layout or {d.id for d in layout.devices}!={d.id for d in self.layout.devices}:
                    raise HostGroupError('layout devices changed')
                if any(self._signature(d)!=self._signature(next(x for x in self.layout.devices if x.id==d.id)) for d in layout.devices):
                    raise HostGroupError('layout screens changed')
                self.layout=layout
                for controller,_session,_remote in self._members.values():controller.layout=layout;controller._event({'type':'layout_changed'})
        return self.authority.while_idle(commit)

    def unregister(self,controller,session):
        self.authority.unregister(controller,session)
        with self._lock:
            for peer,member in tuple(self._members.items()):
                if _same(member,controller,session):del self._members[peer]
            if not self._members:self.local=None;self.layout=None;return
            self._rebuild_locked()
