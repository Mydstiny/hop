"""Isolated GTK + fake discovery + real candidate and pinned-TLS pairing."""
import ctypes as C
import os
from pathlib import Path
import socket
import threading
import time


def run():
    if os.environ.get('HOP_VIRTUAL_X_SERVER')!='1':raise RuntimeError('virtual X server required')
    library=os.environ.get('HOP_TEST_TLS_LIBRARY');fixture=os.environ.get('HOP_TEST_TLS_FIXTURE_DIR')
    if not library or not fixture:print('SKIP: pairing UI requires synthetic TLS fixture');return
    from hop_linux.controller import PairingController
    from hop_linux.discovery import DeviceHint
    from hop_linux.pair_flow import pair_verified_socket
    from hop_linux.pair_offer import exchange_candidate
    from hop_linux.pairing import PendingPairing
    from hop_linux.pair_ui import GtkPairingWindow
    from hop_linux.tls import TLSLibrary,TLSError
    from test_pairing import Store
    from test_session import Sink
    p=Path(fixture);ca=(p/'a.crt').read_bytes();ka=(p/'a.key').read_bytes();cb=(p/'b.crt').read_bytes();kb=(p/'b.key').read_bytes();lib=TLSLibrary(library)
    client_offer,server_offer=socket.socketpair();client_tls,server_tls=socket.socketpair()
    client_sockets=[client_offer,client_tls];hint=DeviceHint('模拟设备','192.168.1.2',41337,1)
    class Discovery:
        def __init__(self):self.events=[{'type':'hints','items':[hint]}]
        def start(self,**kwargs):pass
        def poll(self):return self.events.pop(0) if self.events else None
        def close(self):pass
    approved=threading.Event();end_remote=threading.Event();remote_store=Store();errors=[]
    def remote():
        session=None
        try:
            offer=exchange_candidate(server_offer,own_role=2,name='模拟接收端',certificate=cb,tls_library=lib,explicitly_started=True)
            pending=PendingPairing(cb,offer.certificate,lib.certificate_fingerprint)
            if not approved.wait(8):raise RuntimeError('test confirmation timed out')
            pending.confirm(pending.local_fingerprint,pending.peer_fingerprint)
            session=pair_verified_socket(server_tls,pending=pending,library=lib,own_certificate=cb,private_key=kb,role='receiver',tcp_initiator=False,sink=Sink(),trust_store=remote_store)
            while not end_remote.is_set() and not session.closed:
                try:session.poll(.05)
                except (EOFError,TLSError):break
        except BaseException as exc:errors.append(exc)
        finally:
            if session:session.close()
            server_offer.close();server_tls.close()
    remote_thread=threading.Thread(target=remote,daemon=True);remote_thread.start()
    store=Store();controller=PairingController(name='测试主机',certificate=ca,private_key=ka,tls_library=lib,trust_store=store,discovery_factory=Discovery,connector=lambda _:client_sockets.pop(0))
    window=GtkPairingWindow(controller);window.gtk.gtk_button_clicked.argtypes=[C.c_void_p]
    try:
        window.show();window.iterate()
        window.gtk.gtk_button_clicked(window.widgets['refresh']);window._tick();window.iterate()
        assert len(window.hints)==1 and window.gtk.gtk_widget_get_sensitive(window.widgets['configure'])
        window.gtk.gtk_button_clicked(window.widgets['configure']);deadline=time.monotonic()+10
        while controller.state!='comparison' and time.monotonic()<deadline:window._tick();window.iterate();time.sleep(.01)
        assert controller.state=='comparison', (controller.state,repr(controller.last_error),repr(errors))
        fingerprint_text=window.gtk.gtk_label_get_text(window.fingerprints).decode()
        assert lib.certificate_fingerprint(ca) in fingerprint_text and lib.certificate_fingerprint(cb) in fingerprint_text
        window.gtk.gtk_button_clicked(window.widgets['pause']);approved.set()
        while controller.state!='connected' and time.monotonic()<deadline:window._tick();window.iterate();time.sleep(.01)
        while not remote_store.saved and time.monotonic()<deadline:time.sleep(.01)
        assert controller.state=='connected' and len(store.saved)==1 and len(remote_store.saved)==1
        window.save_virtual_screenshot(p/'pairing-ui-verified.png')
    finally:
        controller.close();window.close();end_remote.set();remote_thread.join(5)
        if remote_thread.is_alive():server_tls.close();raise RuntimeError('remote test thread did not stop')
    if errors:raise RuntimeError('remote pairing test failed') from errors[0]
    print('PASS: isolated GTK discovery, displayed fingerprint confirmation and two-sided pinned TLS pairing')


def run_two_windows():
    if os.environ.get('HOP_VIRTUAL_X_SERVER')!='1':raise RuntimeError('virtual X server required')
    library=os.environ.get('HOP_TEST_TLS_LIBRARY');fixture=os.environ.get('HOP_TEST_TLS_FIXTURE_DIR')
    if not library or not fixture:print('SKIP: two-window pairing requires synthetic TLS fixture');return
    import tempfile
    from hop_linux.controller import PairingController,ReceiverPairingController
    from hop_linux.discovery import DeviceHint
    from hop_linux.pair_ui import GtkPairingWindow,GtkReceiverPairingWindow
    from hop_linux.tls import TLSLibrary
    from hop_linux.profiles import PointerProfile,ProfileStore
    from hop_linux.endpoints import EndpointStore
    from test_pairing import Store
    base=Path(fixture);ca=(base/'a.crt').read_bytes();ka=(base/'a.key').read_bytes();cb=(base/'b.crt').read_bytes();kb=(base/'b.key').read_bytes()
    lib=TLSLibrary(library);path=str(Path(tempfile.mkdtemp(prefix='hop-ui-pair-',dir='/tmp'))/'listener.sock')
    hint=DeviceHint('接收端','192.168.1.2',41337,1)
    class Discovery:
        def __init__(self):self.events=[]
        def start(self,**kwargs):self.events=[{'type':'hints','items':[hint]}] if kwargs.get('port') is None else []
        def poll(self):return self.events.pop(0) if self.events else None
        def close(self):pass
    def listener():
        s=socket.socket(socket.AF_UNIX);s.bind(path);s.listen(4);return s
    def connect(_hint):
        s=socket.socket(socket.AF_UNIX);s.settimeout(5);s.connect(path);return s
    left_store,right_store=Store(),Store()
    left_store.list_peers=lambda:tuple(sorted(pin.lower() for _cert,pin in left_store.saved))
    profile_store=ProfileStore(Path(tempfile.mkdtemp(prefix='hop-ui-profile-',dir='/tmp'))/'profiles.json')
    endpoint_store=EndpointStore(Path(tempfile.mkdtemp(prefix='hop-ui-endpoints-',dir='/tmp'))/'endpoints.json')
    host=PairingController(name='主机',certificate=ca,private_key=ka,tls_library=lib,trust_store=left_store,discovery_factory=Discovery,connector=connect,shortcut_library=os.environ.get('HOP_TEST_SHORTCUT_LIBRARY'),profile_store=profile_store,endpoint_store=endpoint_store)
    receiver=ReceiverPairingController(name='接收端',certificate=cb,private_key=kb,tls_library=lib,trust_store=right_store,discovery_factory=Discovery,listener_factory=listener,enable_input=True)
    left=GtkPairingWindow(host);right=GtkReceiverPairingWindow(receiver)
    try:
        left.show();right.show();left.iterate();right.iterate()
        right.gtk.gtk_button_clicked(right.widgets['refresh'])
        left.gtk.gtk_entry_set_text(left.manual_ip,b'192.168.1.2')
        assert left.gtk.gtk_spin_button_get_value_as_int(left.manual_port)==24891
        left.gtk.gtk_button_clicked(left.manual_button);deadline=time.monotonic()+12
        while (host.state!='comparison' or receiver.state!='comparison') and time.monotonic()<deadline:
            left._tick();right._tick();left.iterate();right.iterate();time.sleep(.01)
        assert host.state==receiver.state=='comparison',(host.state,receiver.state,host.last_error,receiver.last_error)
        right.gtk.gtk_button_clicked(right.widgets['pause']);left.gtk.gtk_button_clicked(left.widgets['pause'])
        while (host.state!='connected' or receiver.state!='connected') and time.monotonic()<deadline:
            left._tick();right._tick();left.iterate();right.iterate();time.sleep(.01)
        assert host.state==receiver.state=='connected',(host.state,receiver.state,host.last_error,receiver.last_error)
        assert len(left_store.saved)==len(right_store.saved)==1
        left.gtk.gtk_button_clicked(left.trusted_list_button)
        while not left.trusted_peers and time.monotonic()<deadline:
            left._tick();right._tick();left.iterate();right.iterate();time.sleep(.01)
        pin=lib.certificate_fingerprint(cb).lower()
        assert left.trusted_peers==[{'pin':pin,'hint':{'name':'接收端','address':'192.168.1.2','port':24891}}]
        assert left.gtk.gtk_entry_get_text(left.trusted_fingerprint).decode()==pin
        assert left.gtk.gtk_entry_get_text(left.manual_ip).decode()=='192.168.1.2'
        while not host.screens and time.monotonic()<deadline:
            left._tick();right._tick();left.iterate();right.iterate();time.sleep(.01)
        assert host.screens and host.screens[0][1:]==(0,0,1280,900)
        assert left.gtk.gtk_widget_get_sensitive(left.profile_button)
        def set_profile(_data):
            dialog,controls,checks=left._profile_widgets
            for control,index in zip(controls[:3],(6,1,7)):left.gtk.gtk_combo_box_set_active(control,index)
            left.gtk.gtk_combo_box_set_active(controls[3],0)
            left.gtk.gtk_toggle_button_set_active(checks[0],True)
            left.gtk.gtk_dialog_response(dialog,-5);return 0
        callback=C.CFUNCTYPE(C.c_int,C.c_void_p)(set_profile)
        left.glib.g_timeout_add(30,C.cast(callback,C.c_void_p),None)
        left.gtk.gtk_button_clicked(left.profile_button)
        while host.profile_pending and time.monotonic()<deadline:time.sleep(.01)
        expected=PointerProfile(2,.5,3,True,False,60)
        assert profile_store.get(lib.certificate_fingerprint(cb))==expected
        if host.shortcut_library:
            # Explicitly enable observation, then synthesize an edge approach on
            # this isolated server. Both devices share Xvfb, so no input is
            # synthesized after remote capture begins.
            from hop_linux.x11 import X11Backend
            while not left.gtk.gtk_widget_get_sensitive(left.edge_button) and time.monotonic()<deadline:
                left._tick();right._tick();left.iterate();right.iterate();time.sleep(.01)
            left._selected()
            assert left.gtk.gtk_widget_get_sensitive(left.edge_button),(host.state,host.layout,host.shortcut_library,host.last_error)
            left.gtk.gtk_button_clicked(left.edge_button)
            while not host.edge_observer and time.monotonic()<deadline:
                left._tick();right._tick();left.iterate();right.iterate();time.sleep(.01)
            assert host.edge_observer is not None
            with X11Backend() as synthetic:
                synthetic.motion(100,300);time.sleep(.15)
                host.edge_observer.events.put({'type':'edge','screen_id':host.local_screens[0][0],'direction':'right','x':65535,'y':20000})
                until=time.monotonic()+.25
                while time.monotonic()<until:
                    left._tick();right._tick();left.iterate();right.iterate();time.sleep(.01)
                assert host.state=='connected' and host.active_capture is None # stale event, pointer no longer at edge
                synthetic.button(1,True);synthetic.motion(1279,300)
                until=time.monotonic()+.4
                while time.monotonic()<until:
                    left._tick();right._tick();left.iterate();right.iterate();time.sleep(.01)
                assert host.state=='connected' and host.active_capture is None
                synthetic.button(1,False);synthetic.motion(100,300);time.sleep(.15);synthetic.motion(1279,300)
            while host.state!='controlling' and time.monotonic()<deadline:
                left._tick();right._tick();left.iterate();right.iterate();time.sleep(.01)
            assert host.state=='controlling',(host.state,host.last_error)
            assert host.active_capture.config['profile']['forward_hz']==60
            assert host.active_capture.config['profile']['movement']==2
            assert host.layout is not None
            # Feed an edge event at the capture IPC boundary; the lower-level
            # native edge detection is tested separately on the private server.
            host.active_capture.events.put(('edge',{'direction':'left','x':0,'y':32768,'clear':False}))
            until=time.monotonic()+.15
            while time.monotonic()<until:
                left._tick();right._tick();left.iterate();right.iterate();time.sleep(.01)
            assert host.state=='controlling' and host.active_capture is not None
            host.active_capture.events.put(('edge',{'direction':'left','x':0,'y':32768,'clear':True}))
            while host.state!='connected' and time.monotonic()<deadline:
                left._tick();right._tick();left.iterate();right.iterate();time.sleep(.01)
            assert host.state=='connected' and host.active_capture is None,(host.state,host.last_error)
    finally:
        left.close();right.close()
        for controller in (host,receiver):
            if controller.worker:controller.worker.join(4)
        if host.shortcut_library:
            from hop_linux.x11 import X11Backend
            with X11Backend() as probe:probe.grab();probe.ungrab()
    # Explicit trusted reconnect uses the saved pin, no new candidate or TOFU.
    path2=str(Path(tempfile.mkdtemp(prefix='hop-ui-reconnect-',dir='/tmp'))/'listener.sock')
    def listener2():
        s=socket.socket(socket.AF_UNIX);s.bind(path2);s.listen(4);return s
    def connect2(_hint):
        s=socket.socket(socket.AF_UNIX);s.settimeout(5);s.connect(path2);return s
    host2=PairingController(name='主机',certificate=ca,private_key=ka,tls_library=lib,trust_store=left_store,discovery_factory=Discovery,connector=connect2)
    receiver2=ReceiverPairingController(name='接收端',certificate=cb,private_key=kb,tls_library=lib,trust_store=right_store,discovery_factory=Discovery,listener_factory=listener2)
    left2=GtkPairingWindow(host2);right2=GtkReceiverPairingWindow(receiver2)
    try:
        left2.show();right2.show();left2.iterate();right2.iterate()
        right2.gtk.gtk_button_clicked(right2.widgets['refresh'])
        left2.gtk.gtk_entry_set_text(left2.manual_ip,b'192.168.1.2')
        left2.gtk.gtk_entry_set_text(left2.trusted_fingerprint,lib.certificate_fingerprint(cb).encode())
        left2.gtk.gtk_button_clicked(left2.trusted_button);deadline=time.monotonic()+8
        while (host2.state!='connected' or receiver2.state!='connected') and time.monotonic()<deadline:
            left2._tick();right2._tick();left2.iterate();right2.iterate();time.sleep(.01)
        assert host2.state==receiver2.state=='connected',(host2.state,receiver2.state,host2.last_error,receiver2.last_error)
        assert len(left_store.saved)==len(right_store.saved)==1
    finally:
        left2.close();right2.close()
        for controller in (host2,receiver2):
            if controller.worker:controller.worker.join(4)
    print('PASS: GTK pairing, explicit capture activation/cleanup and pinned trusted reconnect')


def run_multi_window():
    if os.environ.get('HOP_VIRTUAL_X_SERVER')!='1':raise RuntimeError('virtual X server required')
    library=os.environ.get('HOP_TEST_TLS_LIBRARY');fixture=os.environ.get('HOP_TEST_TLS_FIXTURE_DIR')
    shortcut=os.environ.get('HOP_TEST_SHORTCUT_LIBRARY')
    if not library or not fixture or not shortcut or not (Path(fixture)/'c.crt').is_file():
        print('SKIP: multi-device GTK needs three synthetic identities and native runtimes');return
    import tempfile
    from hop_linux.controller import PairingController,ReceiverPairingController
    from hop_linux.app_ui import GtkHostWorkspace
    from hop_linux.host_group import HostGroup
    from hop_linux.pair_ui import GtkReceiverPairingWindow
    from hop_linux.tls import TLSLibrary
    from test_pairing import Store
    base=Path(fixture);cert=lambda n:(base/(n+'.crt')).read_bytes();key=lambda n:(base/(n+'.key')).read_bytes()
    lib=TLSLibrary(library);group=HostGroup();hosts=[];receivers=[];receiver_windows=[]
    class Discovery:
        def start(self,**_):pass
        def poll(self):return None
        def close(self):pass
    for n in ('b','c'):
        path=str(Path(tempfile.mkdtemp(prefix='hop-multi-',dir='/tmp'))/'peer.sock')
        def listener(path=path):
            s=socket.socket(socket.AF_UNIX);s.bind(path);s.listen(4);return s
        def connector(_hint,path=path):
            s=socket.socket(socket.AF_UNIX);s.settimeout(5);s.connect(path);return s
        receiver=ReceiverPairingController(name='接收端'+n,certificate=cert(n),private_key=key(n),tls_library=lib,trust_store=Store(),discovery_factory=Discovery,listener_factory=listener,enable_input=True)
        host=PairingController(name='主机',certificate=cert('a'),private_key=key('a'),tls_library=lib,trust_store=Store(),discovery_factory=Discovery,connector=connector,shortcut_library=shortcut,host_group=group)
        hosts.append(host);receivers.append(receiver);receiver_windows.append(GtkReceiverPairingWindow(receiver))
    index=[0]
    def factory():
        h=hosts[index[0]];index[0]+=1;return h
    workspace=GtkHostWorkspace(factory);primary=workspace.add_device(primary=True);primary.show()
    primary.gtk.gtk_button_clicked(primary.add_device_button)
    assert len(workspace.windows)==2
    windows=workspace.windows+receiver_windows
    def tick():
        for w in windows:w._tick();w.iterate()
        time.sleep(.01)
    try:
        for w in receiver_windows:w.show();w.gtk.gtk_button_clicked(w.widgets['refresh'])
        deadline=time.monotonic()+20
        for i in range(2):
            left,right=workspace.windows[i],receiver_windows[i]
            left.gtk.gtk_entry_set_text(left.manual_ip,b'192.168.1.2');left.gtk.gtk_button_clicked(left.manual_button)
            while (hosts[i].state!='comparison' or receivers[i].state!='comparison') and time.monotonic()<deadline:tick()
            assert hosts[i].state==receivers[i].state=='comparison',(i,hosts[i].state,receivers[i].state,hosts[i].last_error,receivers[i].last_error)
            left.gtk.gtk_button_clicked(left.widgets['pause']);right.gtk.gtk_button_clicked(right.widgets['pause'])
            while (hosts[i].state!='connected' or not hosts[i].screens) and time.monotonic()<deadline:tick()
            assert hosts[i].state=='connected' and hosts[i].screens
        while (not group.layout or len(group.layout.devices)!=3) and time.monotonic()<deadline:tick()
        assert group.layout and len(group.layout.devices)==3
        ordered=sorted(group.layout.devices[1:],key=lambda d:d.bounds()[0]);first,second=ordered
        first_host=next(h for h in hosts if h.peer_id==first.id);second_host=next(h for h in hosts if h.peer_id==second.id)
        first_ui=workspace.windows[hosts.index(first_host)];first_ui._selected();first_ui.gtk.gtk_button_clicked(first_ui.widgets['configure'])
        while first_host.state!='controlling' and time.monotonic()<deadline:tick()
        assert first_host.state=='controlling',(first_host.state,first_host.last_error)
        first_host.active_capture.events.put(('edge',{'direction':'right','x':65535,'y':32768,'clear':True}))
        while second_host.state!='controlling' and time.monotonic()<deadline:tick()
        assert second_host.state=='controlling' and first_host.active_capture is None,(first_host.state,second_host.state,first_host.last_error,second_host.last_error)
        second_host.active_capture.events.put(('edge',{'direction':'left','x':0,'y':32768,'clear':True}))
        while first_host.state!='controlling' and time.monotonic()<deadline:tick()
        assert first_host.state=='controlling' and second_host.active_capture is None
        first_host.active_capture.events.put(('edge',{'direction':'left','x':0,'y':32768,'clear':True}))
        while (first_host.state!='connected' or first_host.active_capture) and time.monotonic()<deadline:tick()
        assert first_host.state=='connected' and first_host.active_capture is None
        first_ui.gtk.gtk_button_clicked(first_ui.edge_button)
        while (not all(h.edge_observer for h in hosts)) and time.monotonic()<deadline:tick()
        assert all(h.edge_enabled and h.edge_observer for h in hosts)
        from hop_linux.x11 import X11Backend
        with X11Backend() as synthetic:
            synthetic.motion(100,300);time.sleep(.15);synthetic.motion(1279,300)
        while first_host.state!='controlling' and time.monotonic()<deadline:tick()
        assert first_host.state=='controlling' and second_host.active_capture is None
    finally:
        for w in windows:w.close()
        for controller in hosts+receivers:
            if controller.worker:controller.worker.join(4)
    print('PASS: GTK multi-device workspace, shared layout and pinned session edge handoffs')


def run_receiver_dial():
    if os.environ.get('HOP_VIRTUAL_X_SERVER') != '1':
        raise RuntimeError('virtual X server required')
    library = os.environ.get('HOP_TEST_TLS_LIBRARY')
    fixture = os.environ.get('HOP_TEST_TLS_FIXTURE_DIR')
    shortcut = os.environ.get('HOP_TEST_SHORTCUT_LIBRARY')
    if not all((library, fixture, shortcut)):
        print('SKIP: receiver dial test requires synthetic TLS fixture and shortcut runtime')
        return
    import tempfile
    from hop_linux.controller import PairingController,ReceiverPairingController
    from hop_linux.pair_ui import GtkPairingWindow,GtkReceiverPairingWindow
    from hop_linux.tls import TLSLibrary
    from test_pairing import Store

    class Discovery:
        def start(self, **_): pass
        def poll(self): return None
        def close(self): pass

    lib = TLSLibrary(library)
    base = Path(fixture)
    ca, ka = (base/'a.crt').read_bytes(), (base/'a.key').read_bytes()
    cb, kb = (base/'b.crt').read_bytes(), (base/'b.key').read_bytes()
    host_store, receiver_store = Store(), Store()
    with tempfile.TemporaryDirectory(prefix='hop-receiver-dial-', dir='/tmp') as directory:
        address = []

        def listen():
            address.append(str(Path(directory)/f'listener-{len(address)}.sock'))
            connection = socket.socket(socket.AF_UNIX)
            try:
                connection.bind(address[-1])
                connection.listen(4)
                return connection
            except BaseException:
                connection.close()
                raise

        def connect(_):
            connection = socket.socket(socket.AF_UNIX)
            try:
                connection.connect(address[-1])
                return connection
            except BaseException:
                connection.close()
                raise

        host = PairingController(name='Host listener',certificate=ca,private_key=ka,tls_library=lib,
            trust_store=host_store,discovery_factory=Discovery,listener_factory=listen,shortcut_library=shortcut)
        receiver = ReceiverPairingController(name='Receiver dialer',certificate=cb,private_key=kb,tls_library=lib,
            trust_store=receiver_store,connector=connect,enable_input=True)
        left,right = GtkPairingWindow(host),GtkReceiverPairingWindow(receiver)

        def wait(predicate):
            deadline = time.monotonic()+8
            while not predicate() and time.monotonic()<deadline:
                left._tick();right._tick();left.iterate();right.iterate();time.sleep(.01)
            assert predicate(),(host.state,receiver.state,host.last_error,receiver.last_error)

        try:
            left.show();right.show()
            left.gtk.gtk_button_clicked(left.listen_button)
            assert host.listener is not None
            right.gtk.gtk_entry_set_text(right.manual_ip,b'127.0.0.1')
            right.gtk.gtk_button_clicked(right.manual_button)
            wait(lambda:host.state==receiver.state=='comparison')
            left.gtk.gtk_button_clicked(left.widgets['pause'])
            right.gtk.gtk_button_clicked(right.widgets['pause'])
            wait(lambda:host.state==receiver.state=='connected' and host.screens)
            assert host.listener is None and host_store.saved and receiver_store.saved
            left.gtk.gtk_button_clicked(left.widgets['configure'])
            wait(lambda:host.state=='controlling')
            host.request_local_focus()
            wait(lambda:host.state=='connected' and host.active_capture is None)
            host.cancel_pairing();receiver.cancel_pairing()
            wait(lambda:host.worker is None and receiver.worker is None)
            left.gtk.gtk_button_clicked(left.listen_button)
            right.gtk.gtk_entry_set_text(right.trusted_fingerprint,lib.certificate_fingerprint(ca).encode())
            right.gtk.gtk_button_clicked(right.trusted_button)
            wait(lambda:host.state==receiver.state=='connected' and host.screens)
            assert len(host_store.saved)==len(receiver_store.saved)==1
        finally:
            left.close();right.close()
            for controller in (host,receiver):
                if controller.worker:controller.worker.join(4)
    print('PASS: GTK host listener and receiver dialer pair, control X11 and reconnect with the stored pin')
