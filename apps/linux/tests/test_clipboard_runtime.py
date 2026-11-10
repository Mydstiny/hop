import os
import queue
import time
import unittest
from hop_linux import codec
from hop_linux.clipboard import TEXT
from hop_linux.clipboard_runtime import ClipboardRuntime
from hop_linux.session import Session
from test_session import Sink,Transport


class FakeActor:
    def __init__(self,value):
        self.value=value;self.version=1;self.epoch=0;self.owner_self=False
        self.events=queue.Queue();self.replies={};self.ident=0;self.hold_revoke=False;self.revocations=[]
    def _reply(self,**fields):
        self.ident+=1;self.replies[self.ident]={'id':self.ident,'ok':True,**fields};return self.ident
    def focus(self,epoch):
        self.epoch=epoch
        if epoch:return self.version
        ident=self._reply();self.revocations.append(ident)
        if self.hold_revoke:self.replies.pop(ident)
        return ident
    def call(self,op,timeout=1,**_):
        assert op=='version';return {'version':self.version}
    def take(self,ident):return self.replies.pop(ident,None)
    def check_health(self):pass
    def disable_access(self):self.epoch=0
    def request_read(self,targets):
        ident=self._reply(started=not self.owner_self and self.value is not None)
        if not self.owner_self and self.value is not None:
            self.events.put({'type':'read','id':ident,'epoch':self.epoch,'target':TEXT,'data':self.value,'version':self.version})
        return ident
    def cancel_read(self):pass
    def install(self,epoch,target,data,version):
        applied=bool(epoch==self.epoch and version==self.version)
        if applied:self.value=data;self.version+=1;self.owner_self=True
        return self._reply(applied=applied,version=self.version,installed_version=self.version if applied else None)
    def copy(self,value):self.value=value;self.version+=1;self.owner_self=False
    def close(self):self.epoch=0


@unittest.skipUnless(os.environ.get('HOP_TEST_CLIPBOARD_LIBRARY'),'requires shared clipboard runtime')
class ClipboardRuntimeTests(unittest.TestCase):
    def setUp(self):
        path=os.environ['HOP_TEST_CLIPBOARD_LIBRARY'];self.host_actor=FakeActor(b'copied before crossing');self.receiver_actor=FakeActor(b'old target')
        self.host_clip=ClipboardRuntime(path,actor_factory=lambda:self.host_actor,decoder_factory=lambda:None)
        self.receiver_clip=ClipboardRuntime(path,actor_factory=lambda:self.receiver_actor,decoder_factory=lambda:None)
        a,b=Transport(),Transport();a.peer=b;b.peer=a
        self.host=Session(a,codec,Sink(),'host',capabilities=3,clipboard=self.host_clip,clipboard_sender=self.host_clip.sender)
        self.receiver=Session(b,codec,Sink(),'receiver',capabilities=3,clipboard=self.receiver_clip,clipboard_sender=self.receiver_clip.sender)
        self.host.start();self.receiver.poll();self.host.poll();self.receiver.announce_screens([(1,0,0,100,100)]);self.host.poll()
        self.host.enter(1);self.receiver.poll();self.host.poll()
    def tearDown(self):self.host.close();self.receiver.close()
    def cycle(self):
        self.host.poll(.001);self.receiver.poll(.001);self.host_clip.pump(self.host);self.receiver_clip.pump(self.receiver);time.sleep(.001)
    def wait_for(self,predicate):
        end=time.monotonic()+3
        while not predicate() and time.monotonic()<end:self.cycle()
        self.assertTrue(predicate())
    def test_initial_then_bidirectional_local_change(self):
        self.wait_for(lambda:self.receiver_actor.value==b'copied before crossing')
        self.wait_for(lambda:self.receiver.clipboard_send_ready())
        self.receiver_actor.copy(b'new target content')
        self.wait_for(lambda:self.host_actor.value==b'new target content')
    def test_target_change_after_enter_wins_over_initial_snapshot(self):
        self.receiver_actor.copy(b'local target wins')
        self.wait_for(lambda:self.host_actor.value==b'local target wins')
        self.assertEqual(self.receiver_actor.value,b'local target wins')
    def test_revoke_waits_for_actor_ack_before_release_ack(self):
        self.wait_for(lambda:self.receiver.clipboard_send_ready())
        self.receiver_actor.hold_revoke=True;self.host.release();self.receiver.poll()
        self.assertEqual(self.receiver.phase,'releasing');self.assertEqual(self.receiver_actor.epoch,0)
        self.assertGreaterEqual(self.receiver.sink.releases,2)
        ident=self.receiver_actor.revocations[-1];self.receiver_actor.replies[ident]={'id':ident,'ok':True}
        self.wait_for(lambda:self.host.phase=='local')
    def test_deferred_remote_update_rebases_only_on_own_install(self):
        clip=self.receiver_clip;epoch=self.receiver.epoch
        clip.inner.deferred=(epoch,TEXT,bytearray(b'newer remote'),1)
        ident=self.receiver_actor._reply(applied=True,version=2,installed_version=2)
        clip.operations[ident]=('install',epoch,1);clip._drain()
        self.assertEqual(clip.inner.deferred[3],2)
        clip.inner.deferred=(epoch,TEXT,bytearray(b'newer remote'),2)
        ident=self.receiver_actor._reply(applied=True,version=4,installed_version=3)
        clip.operations[ident]=('install',epoch,2);clip._drain()
        self.assertEqual(clip.inner.deferred[3],2)
