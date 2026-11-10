"""Invoked only by xvfb_integration.py after it creates an isolated X server."""
import os
import select
import subprocess
import sys
import time

OWNER = r'''
import random,select,sys,time,zlib,struct
from hop_linux.clipboard import Focus,TEXT,PNG,CHUNK,MAX_TEXT
from hop_linux.clipboard_x11 import ClipboardX11

def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(data,zlib.crc32(kind))&0xffffffff)
def png():
 r=random.Random(1)
 pixels=b''.join(b'\0'+bytes(r.randrange(256) for _ in range(600)) for _ in range(150))
 return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',150,150,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(pixels))+chunk(b'IEND',b'')
a=ClipboardX11(Focus('peer-a',5),lambda *_:None,exclusive_process=True)
original_request=a.owner.request
mixed=[False]
a.atoms['text/uri-list']=a.x.XInternAtom(a.display,b'text/uri-list',False)
def request(requestor,prop,target):
 if mixed[0] and target=='TARGETS':return ('ATOM',('TARGETS',TEXT,'text/uri-list'))
 return original_request(requestor,prop,target)
a.owner.request=request
try:
 a.install_remote('peer-a',5,TEXT,b'v'*MAX_TEXT)
 print('READY',flush=True)
 end=time.monotonic()+20
 while time.monotonic()<end:
  a.pump()
  if select.select([sys.stdin],[],[],0.01)[0]:
   line=sys.stdin.readline().strip()
   if line=='PNG':a.install_remote('peer-a',5,PNG,png());print('PNG_READY',flush=True)
   elif line=='EMPTY':a.install_remote('peer-a',5,TEXT,b'');print('EMPTY_READY',flush=True)
   elif line=='MIXED':mixed[0]=True;a.install_remote('peer-a',5,TEXT,b'file:///tmp/example');print('MIXED_READY',flush=True)
   elif line=='PATH':mixed[0]=False;a.install_remote('peer-a',5,TEXT,b'file:///tmp/example');print('PATH_READY',flush=True)
   elif line=='STOP':break
finally:a.close()
'''


def line_ready(proc, expected):
    if not select.select([proc.stdout], [], [], 5)[0]:
        raise RuntimeError("virtual clipboard owner did not respond")
    text = proc.stdout.readline().strip()
    if text != expected:
        raise RuntimeError("virtual clipboard owner returned " + repr(text))


def run():
    if os.environ.get("HOP_VIRTUAL_X_SERVER") != "1":
        raise RuntimeError("Virtual X server authorization missing")
    from hop_linux.clipboard import CHUNK, MAX_TEXT, Focus, TEXT, PNG
    from hop_linux.clipboard_x11 import ClipboardX11
    owner = subprocess.Popen([sys.executable, "-B", "-c", OWNER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, env=os.environ.copy())
    received = []
    try:
        line_ready(owner, "READY")
        with_closer = ClipboardX11(Focus("peer-a", 5), lambda target, data: received.append((target, data)),
                                   exclusive_process=True, watch_changes=True)
        try:
            try:
                ClipboardX11(Focus("peer-a", 5), lambda *_: None, exclusive_process=True)
                raise AssertionError("Two clipboard Xlib error handlers were installed in one process")
            except RuntimeError as exc:
                if "only one" not in str(exc): raise
            try:
                with_closer.read_local("other", 5, TEXT)
                raise AssertionError("Inactive clipboard peer accepted")
            except PermissionError:
                pass
            if not with_closer.read_preferred("peer-a", 5): raise AssertionError("Missing virtual clipboard owner")
            end = time.monotonic() + 8
            while not received and time.monotonic() < end:
                with_closer.pump(); time.sleep(.01)
            if received != [(TEXT, b"v" * MAX_TEXT)]:
                raise AssertionError("Virtual INCR text transfer did not finish correctly")
            owner.stdin.write("PNG\n"); owner.stdin.flush()
            line_ready(owner, "PNG_READY")
            received.clear()
            if not with_closer.read_preferred("peer-a", 5): raise AssertionError("Missing PNG owner")
            end = time.monotonic() + 8
            while not received and time.monotonic() < end:
                with_closer.pump(); time.sleep(.01)
            if len(received) != 1 or received[0][0] != PNG or len(received[0][1]) <= CHUNK or not received[0][1].startswith(b"\x89PNG\r\n\x1a\n"):
                raise AssertionError("Virtual PNG clipboard transfer failed")
            received.clear()
            if not with_closer.read_local("peer-a", 5, PNG): raise AssertionError("Missing focus-switch test owner")
            with_closer.set_focus(Focus("peer-b", 6))
            end = time.monotonic() + .2
            while time.monotonic() < end: with_closer.pump(); time.sleep(.01)
            if received: raise AssertionError("Clipboard leaked after focus switched")
            with_closer.set_focus(Focus("peer-a", 5))
            owner.stdin.write("EMPTY\n"); owner.stdin.flush()
            line_ready(owner, "EMPTY_READY")
            if not with_closer.read_local("peer-a", 5, TEXT): raise AssertionError("Missing empty-text owner")
            end = time.monotonic() + 8
            while not received and time.monotonic() < end:
                with_closer.pump(); time.sleep(.01)
            if received != [(TEXT, b"")]: raise AssertionError("Virtual empty clipboard text failed")
            owner.stdin.write("MIXED\n");owner.stdin.flush();line_ready(owner,'MIXED_READY');received.clear()
            if not with_closer.read_preferred('peer-a',5):raise AssertionError('Missing mixed-type owner')
            end=time.monotonic()+2
            while with_closer.read_window and time.monotonic()<end:with_closer.pump();time.sleep(.01)
            if with_closer.read_window or received:raise AssertionError('File-list owner was downgraded to text')
            owner.stdin.write("PATH\n");owner.stdin.flush();line_ready(owner,'PATH_READY')
            if not with_closer.read_local('peer-a',5,TEXT):raise AssertionError('Missing pure-text owner')
            end=time.monotonic()+3
            while not received and time.monotonic()<end:with_closer.pump();time.sleep(.01)
            if received!=[(TEXT,b'file:///tmp/example')]:raise AssertionError('Pure text path was incorrectly blocked')
            from hop_linux.clipboard_actor import ClipboardActor,ClipboardActorError
            import queue
            actor=ClipboardActor()
            try:
                actor.focus(5);version=actor.call('version')['version']
                ident=actor.request_read((PNG,TEXT));end=time.monotonic()+4;ack=None;item=None
                while time.monotonic()<end and (ack is None or item is None):
                    with_closer.pump();ack=ack or actor.take(ident)
                    try:item=actor.events.get_nowait()
                    except queue.Empty:pass
                    time.sleep(.01)
                assert ack and ack['started'] and item and item['type']=='read'
                assert item['target']==TEXT and item['data']==b'file:///tmp/example'
                assert not with_closer.install_remote_if_unchanged('peer-a',5,TEXT,b'blocked',with_closer.local_version(),guard=lambda:False)
                ident=actor.install(5,TEXT,b'actor clipboard',actor.call('version')['version']);end=time.monotonic()+3;reply=None
                while reply is None and time.monotonic()<end:with_closer.pump();reply=actor.take(ident);time.sleep(.01)
                assert reply and reply['applied']
                received.clear();assert with_closer.read_local('peer-a',5,TEXT)
                while not received and time.monotonic()<end:with_closer.pump();time.sleep(.01)
                assert received==[(TEXT,b'actor clipboard')]
                revoke=actor.focus(0)
                try:actor.install(5,TEXT,b'stale',version);raise AssertionError('revoked actor accepted write')
                except ClipboardActorError:pass
                end=time.monotonic()+3;reply=None
                while reply is None and time.monotonic()<end:reply=actor.take(revoke);time.sleep(.01)
                assert reply and actor.gate[0]==0
            finally:actor.close()
            version = with_closer.local_version()
            owner.stdin.write("PNG\n"); owner.stdin.flush();line_ready(owner, "PNG_READY")
            if with_closer.install_remote_if_unchanged("peer-a",5,TEXT,b"must-not-overwrite",version):
                raise AssertionError("Clipboard change was overwritten")
            if with_closer.x.XGetSelectionOwner(with_closer.display,with_closer.atoms['CLIPBOARD']) == with_closer.window:
                raise AssertionError("Conflict incorrectly took clipboard ownership")
            version = with_closer.local_version()
            if not with_closer.install_remote_if_unchanged("peer-a",5,TEXT,b"safe-update",version):
                raise AssertionError("Unchanged clipboard could not be updated")
            if with_closer.x.XGetSelectionOwner(with_closer.display,with_closer.atoms['CLIPBOARD']) != with_closer.window:
                raise AssertionError("Expected clipboard ownership not acquired")
        finally:
            with_closer.close()
        owner.stdin.write("STOP\n"); owner.stdin.flush()
        _, errors = owner.communicate(timeout=5)
        if owner.returncode != 0: raise RuntimeError("Virtual clipboard owner failed: " + errors)
    finally:
        if owner.poll() is None:
            owner.terminate()
            try: owner.wait(timeout=5)
            except subprocess.TimeoutExpired: owner.kill(); owner.wait(timeout=5)
    print("PASS: isolated Xvfb clipboard: 2 MiB INCR text, PNG and atomic owner-conflict checks")
