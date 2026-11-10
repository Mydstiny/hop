"""Exercise the Linux host listener with the HarmonyOS fake SDK receiver.

Run only with a freshly built harmony-fake-receiver, disposable certificate
fixtures and trusted local test runtimes. The test overrides X11 capture and
advertisement and runs the two real network adapters on IPv4 loopback.
"""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import time

from hop_linux.controller import ControllerError, PairingController
from hop_linux.shortcut import ShortcutTranslator
from hop_linux.tls import TLSLibrary
from test_pairing import Store


class DiscoveryFixture:
    def start(self, **_): pass
    def poll(self): return None
    def close(self): pass


def loopback_listener():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        listener.bind(('127.0.0.1', 0))
        listener.listen(4)
        return listener
    except BaseException:
        listener.close()
        raise


class SyntheticHost(PairingController):
    def _monitor_snapshot(self, **_):
        return ((1, 0, 0, 1280, 900),)

    def _start_control(self, session, route=None):
        assert session.peer_platform == 4 and session.transport.authenticated
        screen = session.screens[0][0]
        session.enter(screen, 32768, 32768)
        deadline = time.monotonic()+3
        while session.phase=='entering' and time.monotonic()<deadline:
            session.poll(.025)
        assert session.phase=='remote'
        self._event({'type':'controlling'})
        translator = ShortcutTranslator(self.shortcut_library, 3, session.peer_platform)
        try:
            for usage, mods, down in [(0xe0,4,1),(6,4,1),(6,4,0),(0xe0,0,0)]:
                for key in translator.push(usage=usage,modifiers=mods,down=down,repeat=0):
                    session.send_input('key',**key)
            session.send_input('wheel',horizontal=0,vertical=120)
        finally:
            translator.close()
        session.release()
        self.returning_local=True


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--isolated-interop',action='store_true',required=True)
    parser.add_argument('--receiver-tool',type=Path,required=True)
    parser.add_argument('--host-certificate',type=Path,required=True)
    parser.add_argument('--host-key',type=Path,required=True)
    parser.add_argument('--receiver-certificate',type=Path,required=True)
    parser.add_argument('--receiver-key',type=Path,required=True)
    args=parser.parse_args()
    library=TLSLibrary(os.environ['HOP_TEST_TLS_LIBRARY'])
    host_cert=args.host_certificate.read_bytes()
    expected=library.certificate_fingerprint(args.receiver_certificate.read_bytes())
    store=Store()
    host=SyntheticHost(name='Linux fixture',certificate=host_cert,private_key=args.host_key.read_bytes(),tls_library=library,
        trust_store=store,discovery_factory=DiscoveryFixture,listener_factory=loopback_listener,
        shortcut_library=os.environ['HOP_TEST_SHORTCUT_LIBRARY'])
    host.start_listening()
    port=host.listener.getsockname()[1]
    try:
        host.start_listening()
    except ControllerError:
        pass
    else:
        raise AssertionError('a second listener started')
    peer=subprocess.Popen([str(args.receiver_tool),'127.0.0.1',str(port),str(args.receiver_certificate),
                           str(args.receiver_key),str(args.host_certificate)],stdin=subprocess.PIPE)
    try:
        deadline=time.monotonic()+12
        while peer.poll() is None and time.monotonic()<deadline:
            item=host.poll()
            if item is None:
                time.sleep(.005)
                continue
            host.accept_event(item)
            if item['type']=='compare':
                assert item['local']==library.certificate_fingerprint(host_cert) and item['peer']==expected
                host.confirm(True)
            elif item['type']=='screens':
                assert host.listener is None and store.saved==[(args.receiver_certificate.read_bytes(),expected.lower())]
                host.start_control()
            elif item['type']=='returned_local':
                assert peer.stdin.write(b'\1')==1
                peer.stdin.flush()
                assert peer.wait(timeout=5)==0
                print('PASS: Linux host listener HPOP half-close, HPSL, TLS pinning, focus and input with Harmony fake SDK')
                break
            elif item['type'] in ('error','stopped'):
                raise AssertionError((item,host.last_error))
        else:
            raise AssertionError(('Harmony interop did not finish',peer.poll(),host.state,host.last_error))
    finally:
        host.close()
        if peer.poll() is None:
            peer.kill()
            peer.wait(timeout=4)
        peer.stdin.close()
        if host.worker:
            host.worker.join(4)


if __name__=='__main__':
    main()
