import unittest
from collections import deque
from hop_linux.session import FrameDecoder, ProtocolError, Session, encode_frame


from hop_linux import codec as Codec

class Sink:
    def __init__(self): self.events=[]; self.releases=0; self.restores=0
    def inject(self, event, fields): self.events.append((event, fields))
    def enter(self, fields): pass
    def release_all(self): self.releases+=1
    def restore_local(self): self.restores+=1

class Transport:
    def __init__(self): self.authenticated=True; self.inbox=deque(); self.peer=None
    def write_all(self, data, timeout=10): self.peer.inbox.append(data)
    def read_some(self, timeout): return self.inbox.popleft() if self.inbox else None
    def close(self): self.authenticated=False


def pair():
    a,b=Transport(),Transport();a.peer=b;b.peer=a
    host,receiver=Session(a,Codec,Sink(),'host',capabilities=1),Session(b,Codec,Sink(),'receiver',capabilities=1)
    host.start(); receiver.poll(); host.poll()
    receiver.announce_screens([(1,0,0,1920,1080)]); host.poll()
    return host,receiver

class FrameTests(unittest.TestCase):
    def test_wire_compatibility_and_partial_frames(self):
        wire=encode_frame(2,1,3,b'abc')
        self.assertEqual(wire.hex(),'484f5032020200000000000300000000000000010000000000000003616263')
        decoder=FrameDecoder()
        self.assertEqual(decoder.feed(wire[:19]),[])
        self.assertEqual(decoder.feed(wire[19:]),[(2,1,3,b'abc')])
        decoder.finish()
    def test_replay_truncation_and_limits_fail_closed(self):
        d=FrameDecoder(); frame=encode_frame(1,1,1,b'x'); d.feed(frame)
        with self.assertRaises(ProtocolError): d.feed(frame)
        with self.assertRaises(ProtocolError): d.feed(b'')
        d=FrameDecoder();d.feed(frame[:3])
        with self.assertRaises(ProtocolError): d.finish()
        for args in ((4,1,1,b''),(1,1,1,b'x'*4097),(1,0,1,b''),(1,1,0,b'')):
            with self.assertRaises(ProtocolError): encode_frame(*args)

class SessionTests(unittest.TestCase):
    def test_release_waits_for_both_clipboard_barriers(self):
        class Barrier:
            supported_capabilities=2
            def __init__(self):self.ready=False;self.epoch=None
            def focus(self,epoch):self.epoch=epoch
            def barrier_ready(self):return self.ready
            def expect_initial_snapshot(self):pass
            def initial_done(self):pass
            def initial_processed(self):return True
            def close(self):pass
        a,b=Transport(),Transport();a.peer=b;b.peer=a
        left,right=Barrier(),Barrier()
        host=Session(a,Codec,Sink(),'host',capabilities=3,clipboard=left)
        receiver=Session(b,Codec,Sink(),'receiver',capabilities=3,clipboard=right)
        try:
            host.start();receiver.poll();host.poll();receiver.announce_screens([(1,0,0,100,100)]);host.poll()
            host.enter(1);receiver.poll();host.poll();host.release();receiver.poll()
            self.assertEqual((host.phase,receiver.phase,left.epoch,right.epoch),('releasing','releasing',0,0))
            self.assertGreaterEqual(receiver.sink.releases,2)
            with self.assertRaises(ProtocolError):host.enter(1)
            right.ready=True;receiver.poll();host.poll()
            self.assertEqual(receiver.phase,'local');self.assertEqual(host.phase,'releasing')
            left.ready=True;host.poll();self.assertEqual(host.phase,'local')
        finally:host.close();receiver.close()

    def test_release_barrier_timeout_fails_closed(self):
        class Barrier:
            supported_capabilities=2
            def focus(self,epoch):pass
            def barrier_ready(self):return False
            def close(self):pass
        a,b=Transport(),Transport();a.peer=b;b.peer=a;now=[10.0]
        host=Session(a,Codec,Sink(),'host',capabilities=3,clipboard=Barrier(),clock=lambda:now[0])
        receiver=Session(b,Codec,Sink(),'receiver',capabilities=1,clock=lambda:now[0])
        host.start();receiver.poll();host.poll();receiver.announce_screens([(1,0,0,100,100)]);host.poll()
        host.enter(1);receiver.poll();host.poll();host.release();receiver.poll();host.poll()
        now[0]+=2.01
        with self.assertRaises(ProtocolError):host.poll()
        self.assertTrue(host.closed);self.assertGreaterEqual(host.sink.restores,1);receiver.close()

    def test_late_barrier_completion_does_not_bypass_deadline(self):
        class Barrier:
            supported_capabilities=2
            ready=False
            def focus(self,epoch):pass
            def barrier_ready(self):return self.ready
            def close(self):pass
        a,b=Transport(),Transport();a.peer=b;b.peer=a;now=[10.0];barrier=Barrier()
        host=Session(a,Codec,Sink(),'host',capabilities=3,clipboard=barrier,clock=lambda:now[0])
        receiver=Session(b,Codec,Sink(),'receiver',capabilities=1,clock=lambda:now[0])
        host.start();receiver.poll();host.poll();receiver.announce_screens([(1,0,0,100,100)]);host.poll()
        host.enter(1);receiver.poll();host.poll();host.release();receiver.poll()
        barrier.ready=True;now[0]+=2.01
        with self.assertRaises(ProtocolError):host.poll()
        self.assertTrue(host.closed);receiver.close()
    def test_authenticated_ack_input_release_and_reentry(self):
        host,receiver=pair()
        with self.assertRaises(ProtocolError): host.send_input('key', usage=4, modifiers=0, down=1, repeat=0)
        host.enter(1)
        with self.assertRaises(ProtocolError): host.send_input('key', usage=4, modifiers=0, down=1, repeat=0)
        receiver.poll();host.poll();self.assertEqual(host.phase,'remote')
        host.send_input('key', usage=4, modifiers=0, down=1, repeat=0);receiver.poll();self.assertEqual(receiver.sink.events[0][0],'key')
        host.release();self.assertEqual(host.sink.restores,1)
        receiver.poll();host.poll();self.assertEqual(host.phase,'local')
        host.enter(1);receiver.poll();host.poll();self.assertEqual(host.epoch,host.session_epoch+2)
        host.close();receiver.close();self.assertGreaterEqual(receiver.sink.releases,3)
    def test_bad_epoch_releases_and_closes(self):
        host,receiver=pair();host.enter(1);receiver.poll();host.poll()
        receiver.transport.inbox.append(encode_frame(2,3,host.epoch+1,Codec.encode_input('key',usage=4,modifiers=0,down=1,repeat=0)))
        with self.assertRaises(ProtocolError): receiver.poll()
        self.assertTrue(receiver.closed);self.assertFalse(receiver.transport.authenticated)
        self.assertEqual(receiver.sink.events,[]);self.assertGreaterEqual(receiver.sink.releases,2)
        host.close()
    def test_transport_failure_releases_and_restores(self):
        host,receiver=pair();host.enter(1);receiver.poll();host.poll()
        def fail(timeout): raise EOFError('test disconnect')
        receiver.transport.read_some=fail
        with self.assertRaises(EOFError): receiver.poll()
        self.assertTrue(receiver.closed);self.assertGreaterEqual(receiver.sink.restores,1)
        self.assertGreaterEqual(receiver.sink.releases,2);host.close()
    def test_unauthenticated_rejected(self):
        transport=Transport();transport.authenticated=False
        with self.assertRaises(ProtocolError): Session(transport,Codec,Sink(),'host')

    def test_wrong_ack_screen_and_held_state_fail_closed(self):
        host,receiver=pair(); host.enter(1)
        host.transport.inbox.append(encode_frame(1,3,host.epoch,Codec.encode_control('entered',screen_id=2)))
        with self.assertRaises(ProtocolError): host.poll()
        self.assertTrue(host.closed); receiver.close()
        host,receiver=pair(); host.enter(1,modifiers=1)
        with self.assertRaises(ProtocolError): receiver.poll()
        self.assertTrue(receiver.closed); host.close()

    def test_timeout_releases_both_sides(self):
        host,receiver=pair(); host._clock=lambda:host.last_receive+3.1
        with self.assertRaises(ProtocolError): host.poll()
        self.assertTrue(host.closed);self.assertGreaterEqual(host.sink.restores,1)
        receiver.close()
