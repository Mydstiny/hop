import os
import ctypes.util
import time
import unittest
from hop_linux import codec
from hop_linux.clipboard import TEXT
from hop_linux.clipboard_assembler import ClipboardAssembler
from hop_linux.clipboard_session import ClipboardSession,ClipboardSender
from hop_linux.clipboard_sync import ClipboardCoordinator
from hop_linux.session import Session,ProtocolError
from test_session import Sink,Transport

class Stub:
    supported_capabilities=2
    def focus(self,epoch):pass
    def barrier_ready(self):return True
    def expect_initial_snapshot(self):pass
    def initial_done(self):pass
    def initial_processed(self):return True
    def close(self):pass

@unittest.skipUnless(os.environ.get('HOP_TEST_CLIPBOARD_LIBRARY'),'requires shared clipboard runtime')
class ClipboardSessionTests(unittest.TestCase):
    def setUp(self):
        a,b=Transport(),Transport();a.peer=b;b.peer=a
        self.version=[1];self.received=[]
        bridge=ClipboardSession(ClipboardAssembler(os.environ['HOP_TEST_CLIPBOARD_LIBRARY']),ClipboardCoordinator(),lambda:self.version[0])
        self.sender=ClipboardSender()
        self.host=Session(a,codec,Sink(),'host',capabilities=3,clipboard=Stub(),clipboard_sender=self.sender)
        self.receiver=Session(b,codec,Sink(),'receiver',capabilities=3,clipboard=bridge)
        self.host.start();self.receiver.poll();self.host.poll()
        self.receiver.announce_screens([(1,0,0,100,100)]);self.host.poll();self.host.enter(1);self.receiver.poll();self.host.poll()
        self.host.clipboard_ready();self.receiver.poll();self.receiver.clipboard_ready();self.host.poll()
    def tearDown(self):self.host.close();self.receiver.close()
    def send(self,ident,data):
        self.host.send_clipboard_frame(3,codec.encode_clipboard_control('start',transfer_id=ident,format=1,total=len(data)));self.receiver.poll()
        if data:
            self.host.send_clipboard_frame(4,codec.encode_clipboard_chunk(ident,0,data));self.receiver.poll()
        if not self.host._clip_initial_sent:
            self.host.send_clipboard_frame(3,codec.encode_clipboard_control('initial_done'));self.receiver.poll()
    def check(self):
        deadline=time.monotonic()+2
        while time.monotonic()<deadline:
            result=self.receiver.clipboard.poll_apply(lambda *args:self.received.append(args))
            if result!='empty':return result
            time.sleep(.001)
        self.fail('clipboard result timed out')
    def test_authenticated_focus_chunk_transfer_and_conflict(self):
        self.send(1,b'hello');self.assertEqual(self.check(),'applied')
        self.assertEqual(self.received,[(self.host.epoch,TEXT,b'hello',1)])
        self.send(2,b'outdated');self.version[0]=2;self.assertEqual(self.check(),'discarded')
        self.send(3,b'');self.assertEqual(self.check(),'applied')
        self.host.release();self.receiver.poll();self.host.poll()
        with self.assertRaises(ProtocolError):self.host.send_clipboard_frame(3,codec.encode_clipboard_control('start',transfer_id=4,format=1,total=0))
    def test_changed_focus_discards_completed_async_value(self):
        self.send(1,b'secret');self.host.release();self.receiver.poll();self.host.poll()
        self.assertEqual(self.check(),'discarded');self.assertEqual(self.received,[])

    def test_initial_snapshot_preserves_target_change_before_start(self):
        self.version[0]=2 # target changed after focus, before host START
        self.send(1,b'old-host-snapshot');self.assertEqual(self.check(),'discarded')
        self.assertEqual(self.received,[])
        self.send(2,b'later-update');self.assertEqual(self.check(),'applied')
        self.assertEqual(self.received[-1][2],b'later-update')

    def test_receiver_waits_until_initial_value_has_been_processed(self):
        self.assertFalse(self.receiver.clipboard_send_ready())
        self.send(1,b'initial')
        self.assertFalse(self.receiver.clipboard_send_ready())
        self.assertEqual(self.check(),'applied')
        self.assertTrue(self.receiver.clipboard_send_ready())

    def test_initial_done_cannot_overtake_chunks(self):
        start=codec.encode_clipboard_control('start',transfer_id=1,format=1,total=5)
        self.host.send_clipboard_frame(3,start);self.receiver.poll()
        with self.assertRaises(ProtocolError):
            self.host.send_clipboard_frame(3,codec.encode_clipboard_control('initial_done'))
        self.assertTrue(self.host.closed)

    def test_initial_done_is_once_per_focus(self):
        done=codec.encode_clipboard_control('initial_done')
        self.host.send_clipboard_frame(3,done);self.receiver.poll()
        self.assertTrue(self.receiver.clipboard_send_ready())
        self.host._send(3,self.host.epoch,done)
        with self.assertRaises(ProtocolError):self.receiver.poll()
        self.assertTrue(self.receiver.closed)

    def test_initial_snapshot_driver_sends_in_fifo_and_unblocks_receiver(self):
        from hop_linux.clipboard_initial import InitialSnapshot
        requested=[];driver=InitialSnapshot(self.sender,cancel_read=lambda:None)
        driver.begin(self.host,1,requested.append);self.assertEqual(requested,[self.host.epoch])
        self.assertFalse(driver.pump(self.host,1))
        self.assertTrue(driver.resolve(self.host.epoch,TEXT,b'copied before crossing',1))
        while driver.pump(self.host,1):self.receiver.poll()
        self.assertEqual(driver.state,'done');self.assertFalse(self.receiver.clipboard_send_ready())
        self.assertEqual(self.check(),'applied');self.assertTrue(self.receiver.clipboard_send_ready())
        self.assertEqual(self.received[-1][2],b'copied before crossing')

    def test_initial_snapshot_skip_and_stale_owner(self):
        from hop_linux.clipboard_initial import InitialSnapshot
        driver=InitialSnapshot(self.sender,cancel_read=lambda:None);driver.begin(self.host,1,lambda _:None)
        self.assertFalse(driver.resolve(self.host.epoch,TEXT,b'obsolete',2))
        self.assertTrue(driver.pump(self.host,2));self.receiver.poll()
        self.assertTrue(self.receiver.clipboard_send_ready());self.assertEqual(self.received,[])

    def test_initial_snapshot_missing_owner_finishes(self):
        from hop_linux.clipboard_initial import InitialSnapshot
        driver=InitialSnapshot(self.sender,cancel_read=lambda:None);driver.begin(self.host,1,lambda _:False)
        self.assertTrue(driver.pump(self.host,1));self.receiver.poll()
        self.assertEqual(driver.state,'done');self.assertTrue(self.receiver.clipboard_send_ready())

    def test_initial_snapshot_unresponsive_owner_times_out_and_cancels(self):
        from hop_linux.clipboard_initial import InitialSnapshot
        now=[0];canceled=[]
        driver=InitialSnapshot(self.sender,cancel_read=lambda:canceled.append(True),clock=lambda:now[0],read_timeout=2)
        driver.begin(self.host,1,lambda _:True);now[0]=2
        self.assertTrue(driver.pump(self.host,1));self.receiver.poll()
        self.assertEqual(canceled,[True]);self.assertEqual(driver.state,'done')
        self.assertFalse(driver.resolve(self.host.epoch,TEXT,b'late',1))

    def test_initial_snapshot_source_change_cancels_before_done(self):
        from hop_linux.clipboard_initial import InitialSnapshot
        driver=InitialSnapshot(self.sender,cancel_read=lambda:None);driver.begin(self.host,1,lambda _:None)
        self.assertTrue(driver.resolve(self.host.epoch,TEXT,b'x'*100000,1))
        self.assertTrue(driver.pump(self.host,1));self.receiver.poll() # START
        self.assertTrue(driver.pump(self.host,2));self.receiver.poll() # CANCEL
        self.assertTrue(driver.pump(self.host,2));self.receiver.poll() # DONE
        self.assertTrue(self.receiver.clipboard_send_ready());self.assertEqual(self.received,[])

    def test_initial_snapshot_skip_does_not_affect_later_updates(self):
        self.version[0]=2
        self.host.send_clipboard_frame(3,codec.encode_clipboard_control('initial_done'));self.receiver.poll()
        self.send(1,b'normal-update');self.assertEqual(self.check(),'applied')

    def test_pause_drops_inflight_and_requires_new_focus_epoch(self):
        self.sender.start(self.host.epoch,TEXT,b'queued',1)
        payload=codec.encode_clipboard_control('start',transfer_id=1,format=1,total=5)
        self.host.send_clipboard_frame(3,payload) # already in transit
        self.receiver.clipboard_pause()
        self.receiver.poll();self.host.poll()
        self.assertIsNone(self.sender.active)
        self.assertIsNone(self.receiver.clipboard.active_id)
        with self.assertRaises(ProtocolError):self.host.send_clipboard_frame(3,payload)
        with self.assertRaises(ProtocolError):self.receiver.clipboard_ready()
        old=self.host.epoch
        self.host.release();self.receiver.poll();self.host.poll();self.host.enter(1);self.receiver.poll();self.host.poll()
        self.host.clipboard_ready();self.receiver.poll();self.receiver.clipboard_ready();self.host.poll()
        self.host.send_clipboard_frame(3,codec.encode_clipboard_control('initial_done'));self.receiver.poll()
        start=codec.encode_clipboard_control('start',transfer_id=2,format=1,total=3)
        self.host.send_clipboard_frame(3,start);self.receiver.poll()
        self.host._send(4,old,codec.encode_clipboard_chunk(1,0,b'old'));self.receiver.poll()
        self.assertEqual(self.receiver.clipboard.active_id,2)
        self.host.send_clipboard_frame(4,codec.encode_clipboard_chunk(2,0,b'new'));self.receiver.poll()
        self.assertEqual(self.check(),'applied');self.assertEqual(self.received[-1][2],b'new')

    def test_simultaneous_pause_is_idempotent(self):
        self.host.clipboard_pause();self.receiver.clipboard_pause()
        self.host.poll();self.receiver.poll()
        self.assertTrue(self.host._clip_paused and self.receiver._clip_paused)
        self.assertFalse(self.host.closed or self.receiver.closed)

    @unittest.skipUnless(ctypes.util.find_library('gdk_pixbuf-2.0'),'requires GdkPixbuf PNG runtime')
    def test_png_transfer_uses_bounded_decoder(self):
        from hop_linux.png_decoder import BoundedPNGDecoder
        from hop_linux.clipboard_initial import InitialSnapshot
        from test_clipboard import png
        self.host.close();self.receiver.close()
        a,b=Transport(),Transport();a.peer=b;b.peer=a
        bridge=ClipboardSession(ClipboardAssembler(os.environ['HOP_TEST_CLIPBOARD_LIBRARY']),ClipboardCoordinator(png_decoder=BoundedPNGDecoder()),lambda:self.version[0])
        stub=Stub();stub.supported_capabilities=6
        self.sender=ClipboardSender(png_decoder=BoundedPNGDecoder())
        self.host=Session(a,codec,Sink(),'host',capabilities=7,clipboard=stub,clipboard_sender=self.sender)
        self.receiver=Session(b,codec,Sink(),'receiver',capabilities=7,clipboard=bridge)
        self.host.start();self.receiver.poll();self.host.poll();self.receiver.announce_screens([(1,0,0,100,100)]);self.host.poll()
        self.host.enter(1);self.receiver.poll();self.host.poll()
        self.host.clipboard_ready();self.receiver.poll();self.receiver.clipboard_ready();self.host.poll()
        image=png()
        driver=InitialSnapshot(self.sender,cancel_read=lambda:None);driver.begin(self.host,1,lambda _:None)
        self.assertTrue(driver.resolve(self.host.epoch,'image/png',image,1))
        while driver.pump(self.host,1):self.receiver.poll()
        self.assertEqual(self.check(),'applied');self.assertEqual(self.received[0][2],image)


def _clipboard_child(sock,parent_sock,env,report):
    from pathlib import Path
    from hop_linux.tls import SecureSocket,TLSLibrary,VerifiedPeer
    from test_tls import fingerprint
    parent_sock.close();base=Path(env['fixtures']);cert_a=(base/'a.crt').read_bytes();cert_b=(base/'b.crt').read_bytes()
    transport=SecureSocket(TLSLibrary(env['tls']),sock,client=False,own_certificate=cert_b,private_key=(base/'b.key').read_bytes(),peer=VerifiedPeer(cert_a,fingerprint(cert_a)))
    session=None
    try:
        transport.handshake()
        bridge=ClipboardSession(ClipboardAssembler(env['clipboard']),ClipboardCoordinator(),lambda:1)
        session=Session(transport,codec,Sink(),'receiver',capabilities=3,clipboard=bridge)
        deadline=time.monotonic()+8
        while session.phase=='hello' and time.monotonic()<deadline:session.poll(.05)
        session.announce_screens([(1,0,0,100,100)])
        output=[]
        while time.monotonic()<deadline:
            session.poll(.03)
            if session.phase=='remote' and not session._clip_local_ready:session.clipboard_ready()
            if session.clipboard.poll_apply(lambda *args:output.append(args))=='applied':
                report.send(output[0][1:]==(TEXT,b'cross-process clipboard',1));return
        raise RuntimeError('clipboard session timed out')
    except Exception as exc:report.send(repr(exc));raise
    finally:
        if session:session.close()
        else:transport.close()
        report.close()

@unittest.skipUnless(os.environ.get('HOP_TEST_CLIPBOARD_LIBRARY') and os.environ.get('HOP_TEST_TLS_LIBRARY') and os.environ.get('HOP_TEST_TLS_FIXTURE_DIR'),'requires native TLS, clipboard and synthetic credentials')
class ClipboardProcessTests(unittest.TestCase):
    def test_two_process_authenticated_focus_clipboard(self):
        import multiprocessing
        import socket
        from pathlib import Path
        from hop_linux.tls import SecureSocket,TLSLibrary,VerifiedPeer
        from test_tls import fingerprint
        fixture=Path(os.environ['HOP_TEST_TLS_FIXTURE_DIR']);a,b=socket.socketpair()
        incoming,outgoing=multiprocessing.Pipe(False)
        child=multiprocessing.Process(target=_clipboard_child,args=(b,a,{'fixtures':str(fixture),'tls':os.environ['HOP_TEST_TLS_LIBRARY'],'clipboard':os.environ['HOP_TEST_CLIPBOARD_LIBRARY']},outgoing))
        child.start();outgoing.close();b.close()
        cert_a=(fixture/'a.crt').read_bytes();cert_b=(fixture/'b.crt').read_bytes()
        transport=SecureSocket(TLSLibrary(os.environ['HOP_TEST_TLS_LIBRARY']),a,client=True,own_certificate=cert_a,private_key=(fixture/'a.key').read_bytes(),peer=VerifiedPeer(cert_b,fingerprint(cert_b)))
        host=None
        try:
            transport.handshake();host=Session(transport,codec,Sink(),'host',capabilities=3,clipboard=Stub());host.start()
            deadline=time.monotonic()+8
            while (host.phase!='local' or not host.screens) and time.monotonic()<deadline:host.poll(.05)
            host.enter(1)
            while host.phase!='remote' and time.monotonic()<deadline:host.poll(.05)
            host.clipboard_ready()
            while not host._clip_peer_ready and time.monotonic()<deadline:host.poll(.05)
            data=b'cross-process clipboard'
            host.send_clipboard_frame(3,codec.encode_clipboard_control('start',transfer_id=1,format=1,total=len(data)))
            host.send_clipboard_frame(4,codec.encode_clipboard_chunk(1,0,data[:7]))
            host.send_clipboard_frame(4,codec.encode_clipboard_chunk(1,7,data[7:]))
            self.assertTrue(incoming.poll(5));self.assertIs(incoming.recv(),True)
            child.join(3);self.assertEqual(child.exitcode,0)
        finally:
            if host:host.close()
            else:transport.close()
            if child.is_alive():child.terminate();child.join(3)
            incoming.close()
