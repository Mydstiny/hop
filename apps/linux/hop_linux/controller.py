"""Explicit GUI-facing host pairing controller, still development-only.

Network/Secret Service calls run on a dedicated worker. Discovery hints never
establish trust. No physical input capture is started by this controller.
"""
import queue
import json
import ipaddress
import os
import socket
import subprocess
import sys
import threading
import time
from .discovery import DeviceHint
from .discovery_process import DiscoveryProcess
from .pair_offer import exchange_candidate
from .pairing import PendingPairing
from .pair_flow import pair_verified_socket
from .profiles import PointerProfile,ProfileStore
from .layout import Device,Screen,Layout,LayoutStore

class ControllerError(RuntimeError):pass

class PairingController:
    network_role=1

    def __init__(self, *, name, certificate, private_key, tls_library, trust_store,
                 discovery_factory=DiscoveryProcess, connector=None, shortcut_library=None, clipboard_library=None, profile_store=None, layout_store=None, endpoint_store=None, host_group=None,
                 listen_port=24891, listener_factory=None):
        if not tls_library.validate_identity(certificate,private_key):raise ControllerError('invalid local identity')
        self.name,self.certificate,self.private_key=name,certificate,private_key
        self.library,self.store=tls_library,trust_store
        self.discovery_factory=discovery_factory;self.connector=connector or self._connect
        if type(listen_port) is not int or not 1<=listen_port<=65535:raise ValueError('invalid listen port')
        self.listen_port=listen_port;self.listener_factory=listener_factory or self._listener;self.listener=None
        if shortcut_library is not None and not os.path.isabs(os.fspath(shortcut_library)):raise ValueError('absolute shortcut runtime path required')
        if clipboard_library is not None and not os.path.isabs(os.fspath(clipboard_library)):raise ValueError('absolute clipboard runtime path required')
        self.clipboard_library=clipboard_library;self.clipboard_allowed=False;self.clipboard_state='off';self.clipboard_runtime=None
        self.shortcut_library=shortcut_library;self.active_capture=None;self.commands=queue.Queue(maxsize=1)
        self.return_requested=threading.Event();self.connected_name=None
        if profile_store is not None and not isinstance(profile_store,ProfileStore):raise ValueError('invalid profile store')
        self.profile_store=profile_store;self.profile=PointerProfile();self.peer_id=None;self.profile_pending=False
        if layout_store is not None and not isinstance(layout_store,LayoutStore):raise ValueError('invalid layout store')
        self.layout_store=layout_store;self.layout=None;self.local_id=None;self.local_screens=();self.returning_local=False
        from .endpoints import EndpointStore
        if endpoint_store is not None and not isinstance(endpoint_store,EndpointStore):raise ValueError('invalid endpoint store')
        self.endpoint_store=endpoint_store
        from .host_group import HostGroup
        if host_group is not None and not isinstance(host_group,HostGroup):raise ValueError('invalid host group')
        self.host_group=host_group;self._group_registered=False
        self.edge_enabled=False;self.edge_observer=None
        self.listing_peers=False
        self.discovery=None;self.events=queue.Queue(maxsize=32);self.answers=queue.Queue(maxsize=1)
        self.worker=None;self.stopping=threading.Event();self.hints=();self.screens=();self.state='idle'
        self.last_error=None
        self.active_socket=None;self.socket_lock=threading.Lock()

    @staticmethod
    def _connect(hint):return socket.create_connection((hint.address,hint.port),timeout=8)
    def _event(self,item):
        try:self.events.put_nowait(item)
        except queue.Full:self.stopping.set()

    def start_discovery(self):
        if self.discovery is not None or self.worker is not None:raise ControllerError('discovery or connection already started')
        self.discovery=self.discovery_factory()
        try:self.discovery.start(name=self.name,port=None,explicitly_started=True)
        except BaseException:self.discovery.close();self.discovery=None;raise
        self.state='discovering'

    def poll(self):
        discovery=self.discovery
        if discovery:
            item=discovery.poll()
            if item:
                if item.get('type')=='hints':self.hints=tuple(item['items'])
                if item.get('type')=='error':
                    discovery.close()
                    if self.discovery is discovery:self.discovery=None
                    return {'type':'discovery_error'}
                return item
        try:return self.events.get_nowait()
        except queue.Empty:return None

    def begin_pairing(self,hint):
        if self.worker is not None or not isinstance(hint,DeviceHint) or hint not in self.hints:
            raise ControllerError('select a discovered device')
        self._begin_pairing(hint)

    def begin_manual_pairing(self,address,port):
        self._begin_pairing(self._manual_hint(address,port))

    def _manual_hint(self,address,port):
        if self.worker is not None or not isinstance(address,str) or type(port) is not int or not 1<=port<=65535:
            raise ControllerError('invalid manual endpoint')
        try:ip=ipaddress.ip_address(address)
        except ValueError as exc:raise ControllerError('manual address must be numeric') from exc
        if ip.is_unspecified or ip.is_multicast or (ip.version==6 and ip.is_link_local and '%' not in address):
            raise ControllerError('invalid manual endpoint')
        return DeviceHint('手动地址',address,port,0)

    def begin_trusted(self,address,port,fingerprint):
        import re
        hint=self._manual_hint(address,port)
        if not isinstance(fingerprint,str) or not re.fullmatch('[0-9a-fA-F]{64}',fingerprint):raise ControllerError('valid stored fingerprint required')
        self._prepare_connection();self.state='connecting'
        self.worker=threading.Thread(target=self._reconnect,args=(hint,fingerprint),name='hop-reconnect-worker',daemon=True);self.worker.start()

    def _begin_pairing(self,hint):
        self._prepare_connection();self.state='candidate'
        self.worker=threading.Thread(target=self._pair,args=(hint,),name='hop-pairing-worker',daemon=True);self.worker.start()

    def _prepare_connection(self):
        for pending in (self.answers,self.commands):
            while True:
                try:pending.get_nowait()
                except queue.Empty:break
        self.stopping.clear();self.return_requested.clear();self.screens=();self.returning_local=False;self.last_error=None

    def confirm(self,approved):
        if self.state!='comparison' or type(approved) is not bool:raise ControllerError('no fingerprint comparison pending')
        try:self.answers.put_nowait(approved)
        except queue.Full:raise ControllerError('confirmation already submitted')
        self.state='connecting' if approved else 'canceling'

    def start_control(self):
        if self.state!='connected' or not self.screens or not self.shortcut_library or self.profile_pending:raise ControllerError('connected screen and saved profile required')
        try:self.commands.put_nowait('start_control')
        except queue.Full:raise ControllerError('control request already pending')
        self.state='entering'

    def enable_edge_crossing(self,enabled):
        if self.state!='connected' or not self.screens or not self.shortcut_library or type(enabled) is not bool:
            raise ControllerError('connected device required')
        if self.host_group:self.host_group.set_edges_enabled(enabled);return
        try:self.commands.put_nowait('enable_edge' if enabled else 'disable_edge')
        except queue.Full:raise ControllerError('another request is pending')

    def request_local_focus(self):
        self.return_requested.set()

    def refresh_trusted_peers(self):
        if self.listing_peers or not callable(getattr(self.store,'list_peers',None)):
            raise ControllerError('trusted device list unavailable')
        self.listing_peers=True
        def work():
            try:
                pins=self.store.list_peers();hints={}
                if self.endpoint_store:
                    try:hints=self.endpoint_store.all()
                    except (OSError,ValueError):self._event({'type':'endpoints_error'})
                self._event({'type':'trusted_peers','items':tuple({'pin':pin,'hint':hints.get(pin)} for pin in pins)})
            except Exception:self._event({'type':'trusted_peers_error'})
            finally:self.listing_peers=False
        threading.Thread(target=work,name='hop-trusted-peer-list',daemon=True).start()

    def save_profile(self,profile):
        if self.state!='connected' or not self.peer_id or not self.profile_store or self.profile_pending or not isinstance(profile,PointerProfile):
            raise ControllerError('connected device profile required')
        peer=self.peer_id;self.profile_pending=True
        def save():
            try:
                self.profile_store.update(peer,profile)
                if self.peer_id==peer:self.profile=profile
                self._event({'type':'profile_saved'})
            except Exception:self._event({'type':'profile_error'})
            finally:self.profile_pending=False
        threading.Thread(target=save,name='hop-profile-writer',daemon=True).start()

    def _pair(self,hint):
        session=None;pending=None;clipboard=None
        try:
            candidate=exchange_candidate(self._open(hint),own_role=1,name=self.name,certificate=self.certificate,
                                         tls_library=self.library,explicitly_started=True,tcp_initiator=True,timeout=10)
            pending=self._review_candidate(candidate)
            if pending is None:return
            clipboard=self._new_clipboard()
            session=pair_verified_socket(self._open(hint),pending=pending,library=self.library,own_certificate=self.certificate,
                private_key=self.private_key,role='host',tcp_initiator=True,sink=_NoCapture(),trust_store=self.store,canceled=self.stopping.is_set,
                capabilities=1|8|16|(clipboard.supported_capabilities if clipboard else 0),clipboard=clipboard,clipboard_sender=clipboard.sender if clipboard else None)
            self._serve_host_session(session,candidate.name,hint)
        except BaseException as exc:
            self.last_error=type(exc).__name__
            if self.host_group and (self.active_capture or self.returning_local):self.host_group.authority.poison()
            self._event({'type':'error','message':'配对或连接失败，请检查设备与网络。'})
        finally:
            self._close_edge_observer();self.edge_enabled=False
            with self.socket_lock:self.active_socket=None
            if pending:pending.cancel()
            if session:self._finish_host_session(session)
            elif clipboard:
                try:clipboard.close()
                except Exception:pass
            self.active_capture=None
            self._event({'type':'stopped'})

    def _reconnect(self,hint,fingerprint):
        from .reconnect import reconnect_verified_socket
        session=None;clipboard=None
        try:
            if self.store.get_peer(fingerprint) is None:raise ControllerError('peer is not trusted')
            clipboard=self._new_clipboard()
            session=reconnect_verified_socket(self._open(hint),library=self.library,own_certificate=self.certificate,
                private_key=self.private_key,role='host',tcp_initiator=True,sink=_NoCapture(),trust_store=self.store,peer_fingerprint=fingerprint,
                capabilities=1|8|16|(clipboard.supported_capabilities if clipboard else 0),clipboard=clipboard,clipboard_sender=clipboard.sender if clipboard else None)
            self._serve_host_session(session,hint.name,hint)
        except BaseException as exc:
            self.last_error=type(exc).__name__
            if self.host_group and (self.active_capture or self.returning_local):self.host_group.authority.poison()
            if not self.stopping.is_set():self._event({'type':'error','message':'重连失败；请检查信任记录或重新配对。'})
        finally:
            self._close_edge_observer();self.edge_enabled=False
            with self.socket_lock:self.active_socket=None
            if session:self._finish_host_session(session)
            elif clipboard:
                try:clipboard.close()
                except Exception:pass
            self.active_capture=None;self._event({'type':'stopped'})

    def _listener(self):
        sock=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
        try:
            sock.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            sock.bind(('0.0.0.0',self.listen_port));sock.listen(4);return sock
        except BaseException:sock.close();raise

    def start_listening(self):
        if self.worker is not None or self.listener is not None:raise ControllerError('listener or connection already started')
        self._close_discovery()
        self._prepare_connection();self.listener=self.listener_factory()
        try:
            address=self.listener.getsockname()
            port=address[1] if isinstance(address,tuple) else 41337 # isolated test socket
            try:
                self.discovery=self.discovery_factory();self.discovery.start(name=self.name,port=port,explicitly_started=True)
            except Exception:
                self._close_discovery()
                self._event({'type':'discovery_error'})
            self.state='listening';self.worker=threading.Thread(target=self._receive,name='hop-inbound-worker',daemon=True);self.worker.start()
        except BaseException:
            self._close_discovery()
            self.listener.close();self.listener=None;raise

    def _receive(self):
        from .pair_offer import read_connection_prefix
        pending=None;sock=None
        try:
            deadline=time.monotonic()+60;attempts=0;candidate=None;prefix=None
            while not self.stopping.is_set() and time.monotonic()<deadline and attempts<8:
                self.listener.settimeout(max(.01,deadline-time.monotonic()))
                sock=self._remember_socket(self.listener.accept()[0]);attempts+=1
                try:
                    prefix=read_connection_prefix(sock,timeout=min(5,max(.01,deadline-time.monotonic())))
                    if prefix==b'HPOP':
                        candidate=exchange_candidate(sock,own_role=self.network_role,name=self.name,certificate=self.certificate,
                            tls_library=self.library,explicitly_started=True,tcp_initiator=False,timeout=min(10,max(.01,deadline-time.monotonic())),prefix=prefix)
                        sock=None
                    break
                except Exception:
                    sock.close();sock=None
                    if attempts>=8:raise
                    time.sleep(.25)
            if self.stopping.is_set():return
            if prefix is None or (candidate is None and sock is None):raise ControllerError('listener window expired')
            if candidate:
                pending=self._review_candidate(candidate)
                if pending is None:return
                self.listener.settimeout(15);sock=self._remember_socket(self.listener.accept()[0])
            self.listener.close();self.listener=None
            self._close_discovery()
            self._serve_inbound(sock,pending=pending,prefix=prefix,peer_name=candidate.name if candidate else '已配对设备')
        except BaseException as exc:self._inbound_error(exc)
        finally:
            if sock:sock.close()
            self._finish_listener(pending)

    def _review_candidate(self,candidate):
        if self.stopping.is_set():return None
        pending=PendingPairing(self.certificate,candidate.certificate,self.library.certificate_fingerprint)
        try:
            self._event({'type':'compare','name':candidate.name,'local':pending.local_fingerprint,'peer':pending.peer_fingerprint})
            try:approved=self.answers.get(timeout=120)
            except queue.Empty:raise ControllerError('fingerprint comparison timed out')
            if approved and not self.stopping.is_set():
                pending.confirm(pending.local_fingerprint,pending.peer_fingerprint)
                return pending
            self._event({'type':'canceled'})
        except BaseException:
            pending.cancel();raise
        pending.cancel()
        return None

    def _serve_inbound(self,sock, *, pending,prefix,peer_name):
        from .reconnect import reconnect_verified_socket
        session=clipboard=None
        try:
            if self.stopping.is_set():return
            clipboard=self._new_clipboard()
            options=dict(library=self.library,own_certificate=self.certificate,private_key=self.private_key,role='host',
                tcp_initiator=False,sink=_NoCapture(),trust_store=self.store,
                capabilities=1|8|16|(clipboard.supported_capabilities if clipboard else 0),
                clipboard=clipboard,clipboard_sender=clipboard.sender if clipboard else None)
            if pending:session=pair_verified_socket(sock,pending=pending,canceled=self.stopping.is_set,**options)
            else:session=reconnect_verified_socket(sock,prefix=prefix,**options)
            self._serve_host_session(session,peer_name,None)
        except BaseException:
            if self.host_group and (self.active_capture or self.returning_local):self.host_group.authority.poison()
            raise
        finally:
            self._close_edge_observer();self.edge_enabled=False
            if session:self._finish_host_session(session)
            elif clipboard:
                try:clipboard.close()
                except Exception:pass
            self.active_capture=None

    def _inbound_error(self,exc):
        self.last_error=type(exc).__name__
        if not self.stopping.is_set():self._event({'type':'error','message':'接收连接失败；请检查信任记录或重新配对。'})

    def _finish_listener(self,pending):
        with self.socket_lock:self.active_socket=None
        if pending:pending.cancel()
        if self.listener:
            self.listener.close();self.listener=None
        try:self._close_discovery()
        finally:self._event({'type':'stopped'})

    def _close_discovery(self):
        discovery=self.discovery;self.discovery=None
        if discovery:discovery.close()

    def _serve_host_session(self,session,name,hint):
        self.peer_id=session.transport.peer_fingerprint
        if self.host_group:self.host_group.register(self,session);self._group_registered=True
        if self.endpoint_store and hint is not None:
            try:
                previous=self.endpoint_store.get(self.peer_id)
                saved_name=previous['name'] if name=='手动地址' and previous else name
                self.endpoint_store.update(self.peer_id,saved_name,hint.address,hint.port)
            except (OSError,ValueError):self._event({'type':'endpoints_error'})
        profile_error=False
        try:self.profile=self.profile_store.get(self.peer_id) if self.profile_store else PointerProfile()
        except (OSError,ValueError):
            self.profile=PointerProfile();profile_error=True
        self._event({'type':'connected','peer':name})
        if profile_error:self._event({'type':'profile_error'})
        last_screens=None
        while not self.stopping.is_set() and not session.closed:
            try:command=self.commands.get_nowait()
            except queue.Empty:command=None
            if command=='start_control':self._start_control(session)
            if isinstance(command,tuple) and len(command)==3 and command[0]=='start_control':
                if command[1] is session:self._start_control(session,command[2])
                elif self.host_group:self.host_group.authority.abort(self,command[1])
            if command=='enable_edge':self.edge_enabled=True;self._event({'type':'edge_enabled'})
            if command=='disable_edge':self.edge_enabled=False;self._close_edge_observer();self._event({'type':'edge_disabled'})
            if self.return_requested.is_set():
                self.return_requested.clear()
                if session.phase=='remote':
                    session.release();self.active_capture=None;self.returning_local=True
            may_watch=not self.host_group or self.host_group.authority.idle()
            if (not self.edge_enabled or not may_watch) and self.edge_observer:self._close_edge_observer()
            if self.edge_enabled and may_watch and session.phase=='local' and self.layout and self.local_screens and self.screens:
                if self.edge_observer is None:
                    from .edge_process import EdgeProcess
                    self.edge_observer=EdgeProcess(self.local_screens);self.edge_observer.start()
                event=self.edge_observer.poll()
                if event:self._enter_from_local_edge(session,event)
            if self.active_capture:self.active_capture.pump(session)
            session.poll(.01 if self.active_capture else .05)
            self._service_clipboard(session)
            if self.returning_local and session.phase=='local':
                self.returning_local=False
                if self.host_group:
                    pending=self.host_group.authority.released(self,session)
                    if pending:
                        target,target_session,route=pending
                        try:target.commands.put_nowait(('start_control',target_session,route))
                        except queue.Full:self.host_group.authority.abort(target,target_session);target._event({'type':'focus_busy'})
                self._event({'type':'returned_local'})
            if session.screens!=last_screens:
                last_screens=session.screens;self.screens=last_screens
                if last_screens:
                    try:self._prepare_layout(session,name)
                    except Exception:self.layout=None;self._event({'type':'layout_error'})
                    self._event({'type':'screens','count':len(last_screens)})

    def _finish_host_session(self,session):
        try:session.close()
        except Exception:
            if self.host_group:self.host_group.authority.poison()
        finally:
            if self.host_group and self._group_registered:
                self._group_registered=False
                try:self.host_group.unregister(self,session)
                except Exception:self.host_group.authority.poison()

    def _new_clipboard(self):
        if not self.clipboard_allowed or not self.clipboard_library:return None
        from .clipboard_runtime import ClipboardRuntime
        runtime=ClipboardRuntime(self.clipboard_library);self.clipboard_runtime=runtime
        if not self.clipboard_allowed:runtime.disable_access()
        return runtime

    def set_clipboard_allowed(self,allowed):
        if type(allowed) is not bool or not self.clipboard_library:raise ControllerError('clipboard unavailable')
        if allowed and self.worker is not None:raise ControllerError('reconnect to enable clipboard sharing')
        self.clipboard_allowed=allowed
        runtime=self.clipboard_runtime
        if not allowed and runtime:runtime.disable_access()

    def _service_clipboard(self,session):
        state='off'
        if session.clipboard:
            negotiated=bool(session.capabilities & session.peer_capabilities & 6)
            if not self.clipboard_allowed:
                if negotiated and session.phase=='remote' and not session._clip_paused:session.clipboard_pause()
            else:
                session.clipboard.pump(session)
                if not negotiated:state='unavailable'
                elif session.phase=='remote':
                    if session._clip_paused:state='paused'
                    elif session._clip_peer_ready and session._clip_local_ready:state='active'
                    else:state='waiting'
                else:state='standby'
        if self.clipboard_state!=state:
            self.clipboard_state=state;self._event({'type':'clipboard_state','state':state})

    def _close_edge_observer(self):
        observer=self.edge_observer;self.edge_observer=None
        if observer:observer.close()

    def _enter_from_local_edge(self,session,event):
        from .layout_router import route_edge
        try:monitors,position=self._monitor_snapshot(require_input_clear=True,include_pointer=True)
        except ControllerError:return # held inputs/Caps Lock: do not steal their release
        if monitors!=self.local_screens:raise ControllerError('local monitor configuration changed')
        screen=next((s for s in monitors if s[0]==event['screen_id']),None)
        if not screen:return
        _,sx,sy,w,h=screen;px,py=position;direction=event['direction']
        if not (sx<=px<sx+w and sy<=py<sy+h):return
        if not ((direction=='left' and px==sx) or (direction=='right' and px==sx+w-1) or
                (direction=='up' and py==sy) or (direction=='down' and py==sy+h-1)):return
        nx=round((px-sx)*65535/(w-1));ny=round((py-sy)*65535/(h-1))
        route=route_edge(self.layout,self.local_id,str(event['screen_id']),direction,nx,ny)
        if not route or route.device_id!=session.transport.peer_fingerprint:return
        self._close_edge_observer();self._start_control(session,route)

    def _monitor_snapshot(self,require_input_clear=False,include_pointer=False):
        command=[sys.executable,'-B','-m','hop_linux.monitor_probe']
        if require_input_clear:command.append('--require-input-clear')
        if include_pointer:
            if not require_input_clear:raise ValueError('pointer probe requires input-state check')
            command.append('--pointer')
        probe=subprocess.run(command,capture_output=True,text=True,timeout=5)
        if probe.returncode:raise ControllerError('local monitor probe failed')
        value=json.loads(probe.stdout)
        monitors=value.get('screens') if include_pointer and isinstance(value,dict) else value
        if not isinstance(monitors,list) or not monitors:raise ControllerError('no local monitor')
        screens=tuple(tuple(m) for m in monitors)
        if include_pointer:
            position=value.get('pointer')
            if not isinstance(position,list) or len(position)!=2 or any(type(v) is not int for v in position):raise ControllerError('invalid pointer position')
            return screens,tuple(position)
        return screens

    def _prepare_layout(self,session,peer_name):
        self.local_screens=self._monitor_snapshot();self.local_id=self.library.certificate_fingerprint(self.certificate).lower()
        local=Device(self.local_id,self.name,'linux',0,0,tuple(Screen(str(i),x,y,w,h) for i,x,y,w,h in self.local_screens))
        systems={1:'macos',2:'windows',3:'linux',4:'harmonyos'}
        peer_screens=tuple(Screen(str(i),x,y,w,h) for i,x,y,w,h in session.screens)
        peer_x=max(x+w for _,x,y,w,h in self.local_screens)-min(s.x for s in peer_screens)
        peer_y=-min(s.y for s in peer_screens)
        remote=Device(session.transport.peer_fingerprint,peer_name,systems[session.peer_platform],peer_x,peer_y,peer_screens)
        if self.host_group:
            self.host_group.update_screens(self,session,local,remote);return
        proposed=Layout((local,remote))
        if proposed.validate().errors:raise ControllerError('monitor layout overlaps')
        if self.layout_store:
            try:
                saved=self.layout_store.load();existing={d.id:d for d in saved.devices}
                signature=lambda d:{(s.id,s.width,s.height) for s in d.screens}
                if all(d.id in existing and signature(existing[d.id])==signature(d) for d in (local,remote)):
                    proposed=saved
            except FileNotFoundError:pass
            except (OSError,ValueError):self._event({'type':'layout_error'})
        self.layout=proposed

    def set_layout(self,layout):
        if self.state!='connected' or not isinstance(layout,Layout) or layout.validate().errors:raise ControllerError('invalid device layout')
        if not self.local_id or not self.peer_id or not {self.local_id,self.peer_id}.issubset({d.id for d in layout.devices}):
            raise ControllerError('layout does not include the connected devices')
        if self.host_group:self.host_group.set_layout(layout)
        else:self.layout=layout

    def _start_control(self,session,route=None):
        from .shortcut import ShortcutTranslator
        from .x11_host_process import X11HostProcess
        if session.phase!='local' or not session.screens:raise ControllerError('no target screen')
        if route and route.device_id!=session.transport.peer_fingerprint:raise ControllerError('target identity changed')
        monitors=self._monitor_snapshot(require_input_clear=True)
        if self.local_screens and monitors!=self.local_screens:raise ControllerError('local monitor configuration changed')
        _,x,y,w,h=monitors[0]
        target=next((s for s in session.screens if route and str(s[0])==route.screen_id),None) if route else session.screens[0]
        if not target:raise ControllerError('target monitor changed')
        screen_id,_,_,tw,th=target;initial=(route.x,route.y) if route else (32768,32768)
        if self.host_group:
            from .host_group import HostGroupError
            try:self.host_group.authority.claim(self,session,handoff=route is not None)
            except HostGroupError:
                if route:self.host_group.authority.abort(self,session)
                self._event({'type':'focus_busy'});return
        translator=ShortcutTranslator(self.shortcut_library,3,session.peer_platform)
        capture=X11HostProcess(screen_id=screen_id,target_size=(tw,th),capture_center=(x+w//2,y+h//2),initial=initial,translator=translator,profile=self.profile,
            edge_directions=('left','right','up','down') if self.layout else (),on_edge=lambda current,event:self._return_to_local(current,screen_id,event))
        session.sink=capture;self.active_capture=capture
        self._close_edge_observer();session.enter(screen_id,*initial)
        deadline=time.monotonic()+3
        while session.phase=='entering' and not self.stopping.is_set() and time.monotonic()<deadline:session.poll(.05)
        if self.stopping.is_set() or session.phase!='remote':raise ControllerError('remote focus was not acknowledged')
        capture.start(session)
        self._event({'type':'controlling'})

    def _return_to_local(self,session,screen_id,event):
        from .layout_router import route_edge
        if not self.layout or not self.local_id or event.get('clear') is not True:return False
        route=route_edge(self.layout,session.transport.peer_fingerprint,str(screen_id),event['direction'],event['x'],event['y'])
        if not route:return False
        try:monitors=self._monitor_snapshot(require_input_clear=True)
        except ControllerError:return False
        if monitors!=self.local_screens:raise ControllerError('local monitor configuration changed')
        if route.device_id!=self.local_id:
            if not self.host_group or not self.host_group.authority.reserve(self,session,route.device_id,route):return False
            session.release();self.active_capture=None;self.returning_local=True
            return True
        local=next((m for m in self.local_screens if str(m[0])==route.screen_id),None)
        if local is None:raise ControllerError('local display changed')
        _,x,y,w,h=local
        px=x+(route.x*(w-1)+32767)//65535;py=y+(route.y*(h-1)+32767)//65535
        session.release();self.active_capture=None;self.returning_local=True
        result=subprocess.run([sys.executable,'-B','-m','hop_linux.local_pointer',str(px),str(py)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=2)
        if result.returncode:raise ControllerError('local pointer restoration failed')
        return True

    def _open(self,hint):
        if self.stopping.is_set():raise ControllerError('pairing canceled')
        return self._remember_socket(self.connector(hint))

    def _remember_socket(self,sock):
        with self.socket_lock:
            if self.stopping.is_set():sock.close();raise ControllerError('pairing canceled')
            self.active_socket=sock
        return sock

    def accept_event(self,item):
        """Called on GUI thread before rendering an event."""
        if item.get('type')=='compare':self.state='comparison'
        elif item.get('type')=='connected':self.state='connected';self.connected_name=item.get('peer')
        elif item.get('type')=='controlling':self.state='controlling'
        elif item.get('type')=='returned_local':self.state='connected'
        elif item.get('type')=='focus_busy':self.state='connected'
        elif item.get('type') in ('error','canceled','stopped'):
            self.state='idle'
            if item.get('type')=='stopped':
                self.worker=None;self.screens=();self.peer_id=None;self.connected_name=None
                self.clipboard_state='off';self.clipboard_runtime=None

    def close(self):
        self.cancel_pairing()
        self._close_discovery()
        # A bounded socket operation may still be winding down; never block GTK.

    def cancel_pairing(self):
        self.stopping.set()
        if self.clipboard_runtime:self.clipboard_runtime.disable_access()
        with self.socket_lock:
            if self.active_socket and not self.active_capture:
                try:self.active_socket.shutdown(socket.SHUT_RDWR)
                except OSError:pass
        try:self.answers.put_nowait(False)
        except queue.Full:pass
        listener=self.listener
        if listener:
            try:listener.shutdown(socket.SHUT_RDWR)
            except OSError:pass
            listener.close()

class _NoCapture:
    def restore_local(self):pass
    def release_all(self):pass
    def inject(self,*_):raise ControllerError('host cannot inject received input')

class ReceiverPairingController(PairingController):
    """Explicit single-peer receiver with optional X11 input and clipboard."""
    def __init__(self, **kwargs):
        self.enable_input=kwargs.pop('enable_input',False)
        if type(self.enable_input) is not bool:raise ValueError('explicit input option required')
        if self.enable_input:
            from .x11 import xlib_threads_initialized
            if not xlib_threads_initialized():raise ControllerError('XInitThreads must run before GTK/X11 initialization')
        super().__init__(**kwargs)

    network_role=2

    def start_discovery(self):self.start_listening()

    def _serve_inbound(self,sock, *, pending,prefix,peer_name):
        self._serve_receiver(sock,pending=pending,prefix=prefix,tcp_initiator=False,peer_name=peer_name)

    def _pair(self,hint):
        pending=None
        try:
            candidate=exchange_candidate(self._open(hint),own_role=2,name=self.name,certificate=self.certificate,
                tls_library=self.library,explicitly_started=True,tcp_initiator=True)
            pending=self._review_candidate(candidate)
            if pending is None:return
            self._serve_receiver(self._open(hint),pending=pending,tcp_initiator=True,peer_name=candidate.name)
        except BaseException as exc:self._inbound_error(exc)
        finally:self._finish_listener(pending)

    def _reconnect(self,hint,fingerprint):
        try:
            if self.store.get_peer(fingerprint) is None:raise ControllerError('peer is not trusted')
            self._serve_receiver(self._open(hint),tcp_initiator=True,peer_name=hint.name,fingerprint=fingerprint)
        except BaseException as exc:self._inbound_error(exc)
        finally:self._finish_listener(None)

    def _serve_receiver(self,sock, *, tcp_initiator, peer_name, pending=None, prefix=b'', fingerprint=None):
        from .reconnect import reconnect_verified_socket
        session=backend=clipboard=None
        try:
            with self.socket_lock:
                if self.stopping.is_set():return
                self.active_socket=sock
            sink=_NoCapture();capabilities=0;screens=()
            if self.enable_input:
                from .x11 import X11Backend
                from .x11_sink import X11Sink
                backend=X11Backend();screens=backend.screens()
                sink=X11Sink(backend,backend.keymap(),screens);capabilities=1|8|16
            clipboard=self._new_clipboard()
            if clipboard:capabilities|=clipboard.supported_capabilities
            options=dict(library=self.library,own_certificate=self.certificate,private_key=self.private_key,role='receiver',
                tcp_initiator=tcp_initiator,sink=sink,trust_store=self.store,capabilities=capabilities,
                clipboard=clipboard,clipboard_sender=clipboard.sender if clipboard else None)
            if pending:
                session=pair_verified_socket(sock,pending=pending,canceled=self.stopping.is_set,**options)
            else:
                session=reconnect_verified_socket(sock,peer_fingerprint=fingerprint,prefix=prefix,**options)
            if screens:session.announce_screens(screens)
            self._event({'type':'connected','peer':peer_name})
            next_check=time.monotonic()+.5
            while not self.stopping.is_set() and not session.closed:
                session.poll(.05)
                self._service_clipboard(session)
                if backend and time.monotonic()>=next_check:
                    if backend.screens()!=screens:raise ControllerError('monitor configuration changed')
                    next_check=time.monotonic()+.5
        finally:
            if session:
                try:session.close()
                except Exception:pass
            elif clipboard:
                try:clipboard.close()
                except Exception:pass
            if backend:
                try:backend.close()
                except Exception:pass
            sock.close()
