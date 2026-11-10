"""Exercise the actual GTK controllers and clipboard actors on three private X servers."""
import base64
from contextlib import ExitStack, contextmanager
import faulthandler
import json
import os
from pathlib import Path
import random
import select
import socket
import subprocess
import struct
import sys
import tempfile
import time
import zlib

from hop_linux.clipboard import Focus, PNG, TEXT


class Discovery:
    def start(self, **_): pass
    def poll(self): return None
    def close(self): pass


class UnavailableDiscovery(Discovery):
    def start(self, **_): self.failed = True
    def poll(self):
        if self.failed:
            self.failed = False
            return {'type':'error'}
        return None


def decode(value):
    return None if value is None else (value[0], base64.b64decode(value[1], validate=True))


def identity(letter):
    root = Path(os.environ['HOP_TEST_TLS_FIXTURE_DIR'])
    return (root / (letter + '.crt')).read_bytes(), (root / (letter + '.key')).read_bytes()


def make_listener(path):
    listener = socket.socket(socket.AF_UNIX)
    try:
        listener.bind(path)
        listener.listen(4)
        return listener
    except BaseException:
        listener.close()
        raise


def send_reply(result):
    print(json.dumps(result), flush=True)


def receive_command():
    line = sys.stdin.readline()
    return json.loads(line) if line else ['quit']


def fixture_png():
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    random_source=random.Random(1)
    rows=b''.join(b'\0'+random_source.randbytes(600) for _ in range(150))
    header=struct.pack('>IIBBBBB',150,150,8,6,0,0,0)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',header)+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')


def selection_server():
    from hop_linux.clipboard_x11 import ClipboardX11
    captured = []
    clip = ClipboardX11(Focus('test', 1), lambda *value: captured.append(value), exclusive_process=True)
    mixed = False
    original_request = clip.owner.request
    clip.atoms['text/uri-list'] = clip.x.XInternAtom(clip.display, b'text/uri-list', False)
    image = fixture_png()

    def request(requestor, prop, target):
        if mixed and target == 'TARGETS':
            return 'ATOM', ('TARGETS', TEXT, 'text/uri-list')
        return original_request(requestor, prop, target)

    clip.owner.request = request
    serving = True
    send_reply('ready')
    try:
        while True:
            if serving:
                clip.pump()
            if not select.select([sys.stdin], [], [], .005)[0]:
                continue
            command, *args = receive_command()
            if command == 'quit':
                send_reply(None)
                return
            if command == 'write':
                target, value = decode(args[0])
                mixed = bool(args[1])
                clip.install_remote('test', 1, target, value)
            elif command == 'serve':
                serving = bool(args[0])
            elif command == 'image':
                send_reply(base64.b64encode(image).decode())
                continue
            elif command == 'read':
                own_window = clip.x.XGetSelectionOwner(clip.display, clip.atoms['CLIPBOARD'])
                if own_window == clip.window:
                    captured[:] = [(clip.owner.target, clip.owner.data)]
                else:
                    captured.clear()
                    clip.read_preferred('test', 1)
                    end = time.monotonic() + .75
                    while clip.read_window and time.monotonic() < end:
                        clip.pump()
                        time.sleep(.002)
                    clip.cancel_read()
                send_reply([captured[0][0], base64.b64encode(captured[0][1]).decode()] if captured else None)
                continue
            else:
                raise RuntimeError('unknown selection command')
            send_reply(None)
    finally:
        clip.close()


def receiver_app(letter, path):
    faulthandler.dump_traceback_later(8)
    from hop_linux.controller import ReceiverPairingController
    from hop_linux.pair_ui import GtkReceiverPairingWindow
    from hop_linux.tls import TLSLibrary
    from hop_linux.x11 import initialize_xlib_threads
    from test_pairing import Store
    initialize_xlib_threads()

    class ObservedReceiver(ReceiverPairingController):
        runtime = None
        def _new_clipboard(self):
            self.runtime = super()._new_clipboard()
            return self.runtime

    cert, key = identity(letter)
    controller = ObservedReceiver(name='Receiver '+letter, certificate=cert, private_key=key,
        tls_library=TLSLibrary(os.environ['HOP_TEST_TLS_LIBRARY']), trust_store=Store(),
        discovery_factory=UnavailableDiscovery if letter=='b' else Discovery,
        listener_factory=lambda:make_listener(path), enable_input=True,
        clipboard_library=os.environ['HOP_TEST_CLIPBOARD_LIBRARY'])
    window = GtkReceiverPairingWindow(controller)
    window.show()
    faulthandler.cancel_dump_traceback_later()
    send_reply('ready')
    try:
        while True:
            window._tick()
            window.iterate()
            if not select.select([sys.stdin], [], [], .005)[0]:
                continue
            command, *args = receive_command()
            if command == 'quit':
                send_reply(None)
                return
            if command == 'listen':
                window.gtk.gtk_toggle_button_set_active(window.clipboard_option, True)
                window.gtk.gtk_button_clicked(window.widgets['refresh'])
            elif command == 'approve':
                window.gtk.gtk_button_clicked(window.widgets['pause'])
            elif command == 'state':
                runtime = controller.runtime
                send_reply((controller.state, controller.clipboard_state, runtime.epoch if runtime else 0,
                            controller.last_error))
                continue
            elif command == 'revoke':
                window.gtk.gtk_toggle_button_set_active(window.clipboard_option, False)
            elif command == 'kill_actor':
                controller.runtime.actor.process.kill()
            else:
                raise RuntimeError('unknown receiver command')
            send_reply(None)
    finally:
        window.close()
        if controller.worker:
            controller.worker.join(4)


class Child:
    def __init__(self, process):
        self.process = process

    def reply(self, timeout=5):
        if not select.select([self.process.stdout], [], [], timeout)[0]:
            raise AssertionError('isolated clipboard test child timed out')
        line = self.process.stdout.readline()
        if not line:
            raise AssertionError('isolated clipboard test child exited')
        return json.loads(line)

    def call(self, *command):
        self.process.stdin.write(json.dumps(command) + '\n')
        self.process.stdin.flush()
        return self.reply()

    def write(self, value, target=TEXT, mixed=False):
        self.call('write', (target, base64.b64encode(value).decode()), mixed)

    def read(self):
        return decode(self.call('read'))


@contextmanager
def child(display, *args):
    env = {**os.environ, **display, 'NO_AT_BRIDGE':'1', 'GDK_BACKEND':'x11',
           'DBUS_SESSION_BUS_ADDRESS':'unix:path=/tmp/hop-clipboard-unused-dbus', 'GSETTINGS_BACKEND':'memory'}
    process = subprocess.Popen([sys.executable, '-B', str(Path(__file__).resolve()), *args],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
    client = Child(process)
    started = False
    try:
        assert client.reply(timeout=12) == 'ready'
        started = True
        yield client
    finally:
        if process.poll() is None:
            try:
                if started:
                    client.call('quit')
                    process.wait(5)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(5)
        process.stdin.close()
        process.stdout.close()


def run(private_x_server):
    if os.environ.get('HOP_VIRTUAL_X_SERVER') != '1':
        raise RuntimeError('private X servers required')
    library = os.environ.get('HOP_TEST_TLS_LIBRARY')
    fixture = os.environ.get('HOP_TEST_TLS_FIXTURE_DIR')
    shortcut = os.environ.get('HOP_TEST_SHORTCUT_LIBRARY')
    clipboard = os.environ.get('HOP_TEST_CLIPBOARD_LIBRARY')
    if not all((library, fixture, shortcut, clipboard)) or not (Path(fixture) / 'c.crt').is_file():
        print('SKIP: clipboard app requires three synthetic identities and all native runtimes')
        return
    import ctypes as C
    from hop_linux.app_ui import GtkHostWorkspace,GtkTray
    from hop_linux.controller import PairingController
    from hop_linux.host_group import HostGroup
    from hop_linux.panel import _bind
    from hop_linux.tls import TLSLibrary
    from hop_linux.x11 import X11Backend
    from test_pairing import Store
    group = HostGroup()
    lib = TLSLibrary(library)
    ca, ka = identity('a')
    hosts = {}
    peers = {}
    selections = {}

    with ExitStack() as stack:
        host_selection = stack.enter_context(child({}, 'selection'))
        for letter in ('b', 'c'):
            display = stack.enter_context(private_x_server())
            selections[letter] = stack.enter_context(child(display, 'selection'))
            directory = stack.enter_context(tempfile.TemporaryDirectory(prefix='hop-clipboard-app-', dir='/tmp'))
            path = str(Path(directory) / 'hop.sock')
            peers[letter] = stack.enter_context(child(display, 'receiver', letter, path))

            def connector(_hint, path=path):
                connection = socket.socket(socket.AF_UNIX)
                try:
                    connection.settimeout(5)
                    connection.connect(path)
                    return connection
                except BaseException:
                    connection.close()
                    raise

            hosts[letter] = PairingController(name='Host', certificate=ca, private_key=ka, tls_library=lib,
                trust_store=Store(), discovery_factory=Discovery, connector=connector, shortcut_library=shortcut,
                clipboard_library=clipboard, host_group=group)

        workspace = GtkHostWorkspace(iter(hosts.values()).__next__)
        primary = workspace.add_device(primary=True)
        primary.show()
        primary.gtk.gtk_button_clicked(primary.add_device_button)
        tray = GtkTray(primary, workspace=workspace)
        stack.callback(tray.close)
        _bind(primary.gtk, 'gtk_menu_item_activate', None, C.c_void_p)
        windows = dict(zip(hosts, workspace.windows))

        def wait(check, description, timeout=5):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                for window in windows.values():
                    window._tick()
                    window.iterate()
                if check():
                    return
                time.sleep(.01)
            status = {k: (h.state, h.clipboard_state, h.last_error) for k, h in hosts.items()}
            raise AssertionError((description, status, {k:p.call('state') for k,p in peers.items()}))

        def move(letter, direction):
            capture = hosts[letter].active_capture
            capture.events.put(('edge', {'direction':direction, 'x':0 if direction=='left' else 65535,
                                         'y':32768, 'clear':True}))

        try:
            host_selection.write(b'host initial snapshot')
            for letter, window in windows.items():
                selection = selections[letter]
                selection.write(('original '+letter).encode())
                peer = peers[letter]
                peer.call('listen')
                assert not window.gtk.gtk_toggle_button_get_active(window.clipboard_option)
                window.gtk.gtk_toggle_button_set_active(window.clipboard_option, True)
                window.gtk.gtk_entry_set_text(window.manual_ip, b'192.168.1.2')
                window.gtk.gtk_button_clicked(window.manual_button)
                wait(lambda: hosts[letter].state == 'comparison' and peer.call('state')[0] == 'comparison', 'pair comparison')
                window.gtk.gtk_button_clicked(window.widgets['pause'])
                peer.call('approve')
                wait(lambda: hosts[letter].state == 'connected' and hosts[letter].screens, 'paired and screens announced')
                assert selection.read() == (TEXT, ('original '+letter).encode())
            wait(lambda: group.layout and len(group.layout.devices) == 3, 'shared device layout')
            order = sorted(hosts, key=lambda letter:next(d.bounds()[0] for d in group.layout.devices if d.id==hosts[letter].peer_id))
            a, b = order
            left, right = hosts[a], hosts[b]
            first, second = selections[a], selections[b]
            first_ui = windows[a]
            first_ui._selected()
            first_ui.gtk.gtk_button_clicked(first_ui.widgets['configure'])
            wait(lambda: first.read() == (TEXT, b'host initial snapshot'), 'initial snapshot')
            assert second.read() == (TEXT, ('original '+b).encode())

            first.write('接收端复制'.encode())
            wait(lambda: host_selection.read() == (TEXT, '接收端复制'.encode()), 'receiver-to-host UTF-8 update')
            png = base64.b64decode(host_selection.call('image'), validate=True)
            host_selection.write(png, PNG)
            wait(lambda: first.read() == (PNG, png), 'host-to-receiver multiframe PNG')
            first.write(b'', TEXT)
            wait(lambda: host_selection.read() == (TEXT, b''), 'receiver-to-host empty text')
            first.write(b'file:///tmp/test-only', mixed=True)
            # The independent test probe refuses the mixed target too. The
            # host retains its prior value after the actor rejects the source.
            time.sleep(.35)
            assert host_selection.read() == (TEXT, b'')
            first.write(b'file:///tmp/test-only')
            wait(lambda: host_selection.read() == (TEXT, b'file:///tmp/test-only'), 'pure text path')

            host_selection.write(b'in-flight on the first target')
            host_selection.call('serve', False)
            move(a, 'right')
            wait(lambda: right.state == 'controlling' and not left.active_capture, 'first-to-second handoff')
            assert peers[a].call('state')[2] == 0
            second.write(b'second target edited after enter')
            host_selection.call('serve', True)
            wait(lambda: host_selection.read() == (TEXT, b'second target edited after enter'), 'target-local edit wins over delayed initial snapshot')
            assert first.read() == (TEXT, b'file:///tmp/test-only')

            move(b, 'left')
            wait(lambda: left.state == 'controlling' and not right.active_capture, 'second-to-first handoff')
            wait(lambda: first.read() == (TEXT, b'second target edited after enter'), 'fresh snapshot for newly active target')
            peers[a].call('revoke')
            wait(lambda: left.clipboard_state == 'paused', 'peer revocation propagates')
            host_selection.write(b'copy after revocation')
            time.sleep(.35)
            assert first.read() == (TEXT, b'second target edited after enter')
            tray._refresh()
            assert primary.gtk.gtk_widget_get_sensitive(tray.pause_item)
            primary.gtk.gtk_menu_item_activate(tray.pause_item)
            wait(lambda: left.state == 'connected' and not left.active_capture, 'tray returns to local after revoked focus')
            assert not group.edges_enabled
            with X11Backend() as probe:
                probe.grab()
                probe.ungrab()

            second_ui = windows[b]
            second_ui._selected()
            second_ui.gtk.gtk_button_clicked(second_ui.widgets['configure'])
            wait(lambda: right.clipboard_state == 'active', 'second target regains focus')
            peers[b].call('kill_actor')
            wait(lambda: not right.active_capture and right.state == 'idle', 'actor death restores local input')
            with X11Backend() as probe:
                probe.grab()
                probe.ungrab()
        finally:
            for window in windows.values():
                window.close()
            for host in hosts.values():
                if host.worker:
                    host.worker.join(4)

    print('PASS: private three-display GTK app clipboard consent, bidirectional updates, owner conflicts, revocation, focus handoffs and actor failure')


if __name__ == '__main__':
    if os.environ.get('HOP_VIRTUAL_X_SERVER') != '1':
        raise SystemExit('private X server required')
    if sys.argv[1:] == ['selection']:
        selection_server()
    elif len(sys.argv) == 4 and sys.argv[1] == 'receiver':
        receiver_app(*sys.argv[2:])
    else:
        raise SystemExit('expected private clipboard app worker mode')
