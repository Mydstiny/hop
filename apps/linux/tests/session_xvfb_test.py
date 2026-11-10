"""Cross-process authenticated TLS -> v2 session -> isolated XTest input."""
import hashlib
import multiprocessing
import os
from pathlib import Path
import socket
import ssl
import time


def _pin(cert): return hashlib.sha256(ssl.PEM_cert_to_DER_cert(cert.decode().strip())).hexdigest()


def _receiver(sock, parent_sock, library_path, certs, report):
    from hop_linux.tls import SecureSocket, TLSLibrary, VerifiedPeer
    from hop_linux.session import Session
    from hop_linux import codec
    from hop_linux.x11 import X11Backend
    from hop_linux.x11_sink import X11Sink
    parent_sock.close()
    transport=SecureSocket(TLSLibrary(library_path),sock,client=False,own_certificate=certs['b_cert'],private_key=certs['b_key'],peer=VerifiedPeer(certs['a_cert'],_pin(certs['a_cert'])))
    backend=None; session=None
    try:
        transport.handshake()
        backend=X11Backend(); sink=X11Sink(backend,backend.keymap(),[(7,0,0,1280,900)])
        session=Session(transport,codec,sink,'receiver',capabilities=1)
        end=time.monotonic()+8
        while session.phase=='hello' and time.monotonic()<end: session.poll(.05)
        session.announce_screens([(7,0,0,1280,900)])
        while time.monotonic()<end:
            try: session.poll(.05)
            except EOFError:
                report.send(('closed',session.closed));return
        raise RuntimeError('receiver timed out')
    except BaseException as exc:
        report.send(('error',repr(exc)));raise
    finally:
        if session: session.close()
        else: transport.close()
        if backend: backend.close()
        report.close()


def run():
    if os.environ.get('HOP_VIRTUAL_X_SERVER')!='1': raise RuntimeError('virtual X server required')
    library=os.environ.get('HOP_TEST_TLS_LIBRARY'); fixture=os.environ.get('HOP_TEST_TLS_FIXTURE_DIR')
    if not library or not fixture:
        print('SKIP: TLS -> session -> X11 sink requires synthetic TLS fixture');return
    from hop_linux.tls import SecureSocket, TLSLibrary, VerifiedPeer
    from hop_linux.session import Session
    from hop_linux import codec
    from hop_linux.x11 import X11Backend
    from test_session import Sink
    base=Path(fixture); certs={f'{n}_{suffix}':(base/f'{n}.{extension}').read_bytes() for n in ('a','b') for suffix,extension in (('cert','crt'),('key','key'))}
    conn,peer=socket.socketpair()
    incoming,outgoing=multiprocessing.Pipe(False)
    child=multiprocessing.Process(target=_receiver,args=(peer,conn,library,certs,outgoing));child.start();outgoing.close();peer.close()
    host_socket=SecureSocket(TLSLibrary(library),conn,client=True,own_certificate=certs['a_cert'],private_key=certs['a_key'],peer=VerifiedPeer(certs['b_cert'],_pin(certs['b_cert'])))
    host=None
    try:
        host_socket.handshake();host=Session(host_socket,codec,Sink(),'host',capabilities=1)
        host.start();end=time.monotonic()+8
        while (host.phase!='local' or not host.screens) and time.monotonic()<end: host.poll(.05)
        assert host.screens and host.screens[0][0]==7
        with X11Backend() as capture:
            a_key=capture.keymap().to_x11(4);capture.grab()
            host.enter(7)
            while host.phase!='remote' and time.monotonic()<end: host.poll(.05)
            assert host.phase=='remote'
            host.send_input('key',usage=4,modifiers=0,down=1,repeat=0)
            received=[]
            while time.monotonic()<end:
                received.extend(capture.events())
                if any(e.kind=='key' and e.code==a_key and e.down for e in received): break
                time.sleep(.01)
            else: raise AssertionError('authenticated synthetic key press not observed')
            host.close()
            while time.monotonic()<end:
                received.extend(capture.events())
                if any(e.kind=='key' and e.code==a_key and not e.down for e in received): break
                time.sleep(.01)
            else: raise AssertionError('disconnect did not release synthetic key')
            capture.ungrab()
        assert incoming.poll(3) and incoming.recv()==('closed',True)
        child.join(3);assert child.exitcode==0
    finally:
        if host: host.close()
        else: host_socket.close()
        if child.is_alive(): child.terminate();child.join(3)
        incoming.close()
    print('PASS: separate processes, pinned mutual TLS, v2 handshake, focus, X11 injection and disconnect key release')


def run_host():
    """Real captured XTest key -> helper IPC -> pinned TLS -> session receiver."""
    if os.environ.get('HOP_VIRTUAL_X_SERVER')!='1': raise RuntimeError('virtual X server required')
    library=os.environ.get('HOP_TEST_TLS_LIBRARY'); fixture=os.environ.get('HOP_TEST_TLS_FIXTURE_DIR')
    if not library or not fixture:
        print('SKIP: X11 host -> TLS session requires synthetic TLS fixture');return
    from hop_linux.tls import SecureSocket, TLSLibrary, VerifiedPeer
    from hop_linux.session import Session
    from hop_linux.x11_host_process import X11HostProcess
    from hop_linux.x11 import X11Backend
    from hop_linux import codec
    from test_session import Sink
    base=Path(fixture);certs={f'{n}_{suffix}':(base/f'{n}.{extension}').read_bytes() for n in ('a','b') for suffix,extension in (('cert','crt'),('key','key'))}
    a,b=socket.socketpair();lib=TLSLibrary(library)
    client=SecureSocket(lib,a,client=True,own_certificate=certs['a_cert'],private_key=certs['a_key'],peer=VerifiedPeer(certs['b_cert'],_pin(certs['b_cert'])))
    server=SecureSocket(lib,b,client=False,own_certificate=certs['b_cert'],private_key=certs['b_key'],peer=VerifiedPeer(certs['a_cert'],_pin(certs['a_cert'])))
    host=receiver=None
    try:
        end=time.monotonic()+8
        while not (client.authenticated and server.authenticated) and time.monotonic()<end:
            if not client.authenticated:client.handshake_step()
            if not server.authenticated:server.handshake_step()
        assert client.authenticated and server.authenticated
        capture=X11HostProcess(screen_id=7,target_size=(1280,900),capture_center=(640,450))
        host=Session(client,codec,capture,'host',capabilities=1)
        receiver=Session(server,codec,Sink(),'receiver',capabilities=1)
        host.start();receiver.poll(1);host.poll(1);receiver.announce_screens([(7,0,0,1280,900)]);host.poll(1)
        host.enter(7);receiver.poll(1);host.poll(1)
        capture.start(host)
        with X11Backend() as sender:
            mapping=sender.keymap();sender.key_hid(mapping,4,True);sender.key_hid(mapping,4,False)
            while time.monotonic()<end:
                capture.pump(host);receiver.poll(.01)
                if len(receiver.sink.events)>=2: break
            assert [event for event,_ in receiver.sink.events]==['key','key']
            assert [fields['down'] for _,fields in receiver.sink.events]==[1,0]
        host.close()
        assert capture.process is None
        with X11Backend() as probe: probe.grab();probe.ungrab()
        try:receiver.poll(1)
        except EOFError:pass
        assert receiver.closed
    finally:
        if host:host.close()
        else:client.close()
        if receiver:receiver.close()
        else:server.close()
    print('PASS: isolated X11 host capture -> helper IPC -> pinned TLS -> v2 input receiver and cleanup')


def _clipboard_receiver(sock,parent_sock,library,clipboard_library,certs,report):
    from hop_linux.tls import SecureSocket,TLSLibrary,VerifiedPeer
    from hop_linux.session import Session
    from hop_linux import codec
    from hop_linux.clipboard import Focus
    from hop_linux.clipboard_x11 import ClipboardX11
    from hop_linux.clipboard_assembler import ClipboardAssembler
    from hop_linux.clipboard_sync import ClipboardCoordinator
    from hop_linux.clipboard_session import ClipboardSession
    from hop_linux.png_decoder import BoundedPNGDecoder
    from test_session import Sink
    parent_sock.close();transport=SecureSocket(TLSLibrary(library),sock,client=False,own_certificate=certs['b_cert'],private_key=certs['b_key'],peer=VerifiedPeer(certs['a_cert'],_pin(certs['a_cert'])))
    session=None;clip=None
    try:
        transport.handshake()
        clip=ClipboardX11(Focus('peer',1),lambda *_:None,exclusive_process=True,watch_changes=True)
        bridge=ClipboardSession(ClipboardAssembler(clipboard_library),ClipboardCoordinator(png_decoder=BoundedPNGDecoder()),clip.local_version)
        session=Session(transport,codec,Sink(),'receiver',capabilities=7,clipboard=bridge)
        deadline=time.monotonic()+10
        while session.phase=='hello' and time.monotonic()<deadline:session.poll(.02)
        session.announce_screens([(7,0,0,1280,900)])
        active=None
        while time.monotonic()<deadline:
            try:session.poll(.01)
            except EOFError:return
            if session.phase=='remote' and active!=session.epoch:
                active=session.epoch;clip.set_focus(Focus('peer',active))
                session.clipboard_ready()
            clip.pump()
            applied=[]
            def install(epoch,target,data,version):
                result=clip.install_remote_if_unchanged('peer',epoch,target,data,version)
                if result:applied.append(target)
                return result
            result=bridge.poll_apply(install)
            if result=='applied':report.send('applied:'+applied[0])
        raise RuntimeError('virtual clipboard session timed out')
    except BaseException as exc:
        report.send('error:'+repr(exc));raise
    finally:
        if session:session.close()
        else:transport.close()
        if clip:clip.close()
        report.close()


def _initial_selection_source(control, report, data):
    from hop_linux.clipboard import Focus,TEXT
    from hop_linux.clipboard_x11 import ClipboardX11
    clip=ClipboardX11(Focus('source',1),lambda *_:None,exclusive_process=True)
    try:
        clip.install_remote('source',1,TEXT,data);report.send(True)
        while not control.poll(.01):clip.pump()
    finally:clip.close();control.close();report.close()


def run_clipboard():
    if os.environ.get('HOP_VIRTUAL_X_SERVER')!='1':raise RuntimeError('virtual X server required')
    library=os.environ.get('HOP_TEST_TLS_LIBRARY');clipboard_library=os.environ.get('HOP_TEST_CLIPBOARD_LIBRARY');fixture=os.environ.get('HOP_TEST_TLS_FIXTURE_DIR')
    if not library or not clipboard_library or not fixture:
        print('SKIP: TLS -> X11 clipboard requires native synthetic fixture');return
    from hop_linux.tls import SecureSocket,TLSLibrary,VerifiedPeer
    from hop_linux.session import Session
    from hop_linux import codec
    from hop_linux.clipboard import Focus,TEXT,PNG
    from hop_linux.clipboard_x11 import ClipboardX11
    from hop_linux.clipboard_session import ClipboardSender
    from hop_linux.clipboard_initial import InitialSnapshot
    from hop_linux.png_decoder import BoundedPNGDecoder
    from test_session import Sink
    from test_clipboard_session import Stub
    from test_clipboard import png
    base=Path(fixture);certs={f'{n}_{suffix}':(base/f'{n}.{extension}').read_bytes() for n in ('a','b') for suffix,extension in (('cert','crt'),('key','key'))}
    source_control,source_end=multiprocessing.Pipe();source_report,source_report_end=multiprocessing.Pipe(False)
    data=b'private synthetic clipboard'
    source=multiprocessing.Process(target=_initial_selection_source,args=(source_end,source_report_end,data));source.start()
    source_end.close();source_report_end.close()
    conn,peer=socket.socketpair();incoming,outgoing=multiprocessing.Pipe(False)
    child=multiprocessing.Process(target=_clipboard_receiver,args=(peer,conn,library,clipboard_library,certs,outgoing));child.start();outgoing.close();peer.close()
    transport=SecureSocket(TLSLibrary(library),conn,client=True,own_certificate=certs['a_cert'],private_key=certs['a_key'],peer=VerifiedPeer(certs['b_cert'],_pin(certs['b_cert'])))
    host=None;reader=None
    try:
        assert source_report.poll(3) and source_report.recv() is True
        stub=Stub();stub.supported_capabilities=6;sender=ClipboardSender(png_decoder=BoundedPNGDecoder())
        transport.handshake();host=Session(transport,codec,Sink(),'host',capabilities=7,clipboard=stub,clipboard_sender=sender);host.start();deadline=time.monotonic()+8
        while (host.phase!='local' or not host.screens) and time.monotonic()<deadline:host.poll(.05)
        host.enter(7)
        while host.phase!='remote' and time.monotonic()<deadline:host.poll(.05)
        host.clipboard_ready()
        while not host._clip_peer_ready and time.monotonic()<deadline:host.poll(.05)
        received=[];reader=ClipboardX11(Focus('peer',host.epoch),lambda target,value:received.append((target,value)),exclusive_process=True,watch_changes=True)
        source_version=reader.local_version();driver=InitialSnapshot(sender,cancel_read=reader.cancel_read)
        driver.begin(host,source_version,lambda epoch:reader.read_preferred('peer',epoch))
        while not received and time.monotonic()<deadline:reader.pump();time.sleep(.01)
        assert received==[(TEXT,data)]
        assert driver.resolve(host.epoch,TEXT,received[0][1],reader.local_version())
        while driver.pump(host,reader.local_version()):pass
        assert incoming.poll(4) and incoming.recv()=='applied:'+TEXT
        received.clear()
        assert reader.read_local('peer',host.epoch,TEXT)
        while not received and time.monotonic()<deadline:reader.pump();time.sleep(.01)
        assert received==[(TEXT,data)]
        image=png();sender.start(host.epoch,PNG,image,1)
        while True:
            frame=sender.next_frame(host.epoch,1)
            if frame is None:break
            host.send_clipboard_frame(*frame)
        assert incoming.poll(4) and incoming.recv()=='applied:'+PNG
        received.clear();assert reader.read_local('peer',host.epoch,PNG)
        while not received and time.monotonic()<deadline:reader.pump();time.sleep(.01)
        assert received==[(PNG,image)]
        sender.close();reader.close();reader=None;host.close();child.join(3);assert child.exitcode==0
    finally:
        if reader:reader.close()
        if host:host.close()
        else:transport.close()
        if child.is_alive():child.terminate();child.join(3)
        try:source_control.send('stop')
        except (BrokenPipeError,EOFError):pass
        source.join(3)
        if source.is_alive():source.terminate();source.join(3)
        source_control.close();source_report.close()
        incoming.close()
    print('PASS: isolated X11 source snapshot -> pinned TLS -> X11 selection, plus PNG transfer')
