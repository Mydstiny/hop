"""Optional isolated Xvfb integration test. NEVER opens the user's desktop.

Requires an explicit Xvfb executable and private directory. The script starts
one new server, obtains an unused display through -displayfd, configures a fresh
Xauthority cookie, and terminates only that server. It injects synthetic input
only into this newly created server. Run with --run-virtual-x11-test.
"""
from contextlib import contextmanager
import os
import faulthandler
import secrets
import select
import stat
import subprocess
import sys
import time
faulthandler.enable()
from pathlib import Path

app_only = sys.argv[1:] == ["--run-clipboard-app-test"]
if not app_only and sys.argv[1:] != ["--run-virtual-x11-test"]:
    raise SystemExit("Explicit --run-virtual-x11-test required")
exe = Path(os.environ.get("HOP_TEST_XVFB", ""))
private = Path(os.environ.get("HOP_TEST_PRIVATE", ""))
if not exe.is_absolute() or exe.name != "Xvfb" or not exe.is_file():
    raise SystemExit("HOP_TEST_XVFB must point to a specific Xvfb executable")
if not private.is_absolute() or not private.is_dir():
    raise SystemExit("HOP_TEST_PRIVATE must be an existing isolated absolute directory")
socket_dir = Path("/tmp/.X11-unix")
if socket_dir.exists() and stat.S_IMODE(socket_dir.stat().st_mode) != 0o1777:
    # Xvfb -displayfd otherwise retries names continuously on WSLg mounts whose
    # socket directory permissions are incompatible with Xorg. Never chmod it.
    raise SystemExit("X11 socket directory has incompatible permissions; cannot safely start isolated Xvfb")
@contextmanager
def private_x_server():
    auth = private / ("xauthority-" + secrets.token_hex(8))
    # Creating a private fresh path avoids modifying any existing Xauthority file.
    auth.touch(mode=0o600, exist_ok=False)
    read_fd, write_fd = os.pipe()
    server_env = os.environ.copy()
    server_env.pop("DISPLAY", None)
    server_env.pop("WAYLAND_DISPLAY", None)
    server_env.pop("XDG_SESSION_TYPE", None)
    server_env["XAUTHORITY"] = str(auth)
    if os.environ.get("HOP_TEST_LIBDIR"):
        server_env["LD_LIBRARY_PATH"] = os.environ["HOP_TEST_LIBDIR"]
    if os.environ.get("HOP_TEST_BINDIR"):
        server_env["PATH"] = os.environ["HOP_TEST_BINDIR"] + os.pathsep + server_env.get("PATH", "")
    log = private / ("xvfb-" + secrets.token_hex(8) + ".log")
    with log.open("wb") as log_stream:
        process = subprocess.Popen([str(exe), "-displayfd", str(write_fd), "-nolisten", "tcp", "-auth", str(auth),
                                    "-screen", "0", "1280x900x24", "-fp", "built-ins"],
                                   pass_fds=(write_fd,), env=server_env, stdout=log_stream, stderr=log_stream)
        os.close(write_fd)
        try:
            if not select.select([read_fd], [], [], 15)[0]:
                raise RuntimeError("Xvfb did not allocate a display")
            raw = os.read(read_fd, 64).decode("ascii").strip()
            if not raw.isdecimal() or process.poll() is not None:
                raise RuntimeError("Xvfb failed to start; inspect " + str(log))
            display = ":" + raw
            cookie = secrets.token_hex(16)
            subprocess.run(["/usr/bin/xauth", "-f", str(auth), "add", display, ".", cookie],
                           env=server_env, check=True, timeout=10)
            yield dict(DISPLAY=display, XAUTHORITY=str(auth))
        finally:
            os.close(read_fd)
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


with private_x_server() as display_env:
    # Only our newly allocated server is ever exposed to the backend.
    os.environ.update(display_env)
    os.environ.pop("WAYLAND_DISPLAY", None)
    os.environ.pop("XDG_SESSION_TYPE", None)
    # Xlib is used from a dedicated capture thread below. This must occur
    # before any Xlib connection in this test process is opened.
    from hop_linux.x11 import X11Backend,initialize_xlib_threads
    initialize_xlib_threads()
    if app_only:
        os.environ['HOP_VIRTUAL_X_SERVER']='1'
        from clipboard_app_virtual_test import run as run_clipboard_app
        run_clipboard_app(private_x_server)
        raise SystemExit(0)
    with X11Backend() as capture, X11Backend() as injector:
        monitors=capture.screens()
        if len(monitors)!=1 or monitors[0][1:]!=(0,0,1280,900):
            raise AssertionError('Unexpected isolated RandR monitor geometry: '+repr(monitors))
        # Edge observation is an independent read-only process and does not
        # claim the pointer or keyboard. All movement is on this server.
        import json
        watcher=subprocess.Popen([sys.executable,'-B','-m','hop_linux.edge_worker'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
        try:
            if not select.select([watcher.stdout],[],[],3)[0]:raise AssertionError('edge observer did not start')
            assert json.loads(watcher.stdout.readline())['type']=='ready'
            injector.motion(100,100);time.sleep(.1)
            assert capture.pointer_position()==(100,100)
            injector.motion(1279,100)
            if not select.select([watcher.stdout],[],[],3)[0]:raise AssertionError('edge approach not observed')
            edge=json.loads(watcher.stdout.readline())
            assert (edge['type'],edge['screen_id'],edge['direction'],edge['x'])==('edge',monitors[0][0],'right',65535)
        finally:
            watcher.stdin.close()
            watcher.wait(timeout=3)
        assert watcher.returncode==0
        print('PASS: isolated read-only local edge approach observer exits on parent pipe closure')
        keymap = capture.keymap()
        a_key = keymap.to_x11(0x04)
        left_ctrl = keymap.to_x11(0xE0)
        right_ctrl = keymap.to_x11(0xE4)
        if a_key is None or left_ctrl is None or right_ctrl is None or left_ctrl == right_ctrl:
            raise AssertionError("The isolated X server did not expose expected distinct XKB physical names")
        capture.grab()
        injector.motion(40, 50)
        injector.key_hid(keymap, 0x04, True); injector.key_hid(keymap, 0x04, False)
        injector.key_hid(keymap, 0xE0, True); injector.key_hid(keymap, 0xE4, True)
        injector.key_hid(keymap, 0xE0, False); injector.key_hid(keymap, 0xE4, False)
        injector.button(1, True); injector.button(1, False)
        injector.scroll(1, -1)
        captured = []
        end = time.monotonic() + 5
        while time.monotonic() < end:
            captured.extend(capture.events())
            if (any(e.kind == "key" and e.code == a_key and e.down for e in captured) and
                any(e.kind == "key" and e.code == a_key and not e.down for e in captured) and
                all(any(e.kind == "key" and e.code == code and e.down == action for e in captured)
                    for code in (left_ctrl, right_ctrl) for action in (True, False)) and
                any(e.kind == "button" and e.code == 1 and e.down for e in captured) and
                any(e.kind == "button" and e.code == 1 and not e.down for e in captured) and
                any(e.kind == "button" and e.code == 7 and e.down for e in captured) and
                any(e.kind == "button" and e.code == 4 and e.down for e in captured)):
                break
            time.sleep(0.02)
        else:
            raise AssertionError("Expected XTest events were not observed on the isolated Xvfb server: " + repr(captured))
        from hop_linux.x11_sink import X11Sink
        sink = X11Sink(injector, keymap, [(7, 0, 0, 1280, 900)])
        sink.enter(dict(screen_id=7,x=32768,y=32768,modifiers=0,buttons=0))
        sink.inject('key',dict(usage=4,modifiers=0,down=1,repeat=0))
        sink.release_all()  # Disconnect cleanup releases the injected key.
        received=[]; end=time.monotonic()+3
        while time.monotonic()<end:
            received.extend(capture.events())
            if all(any(e.kind=='key' and e.code==a_key and e.down==down for e in received) for down in (True,False)): break
            time.sleep(.01)
        else: raise AssertionError('X11 session sink did not release synthetic key on cleanup')
        capture.ungrab()
    print("PASS: isolated Xvfb XKB/HID mapping, X11 grab, XTest input and X11 session sink cleanup")
    from hop_linux.x11_host import X11Host
    from test_x11_host import FakeSession
    host=X11Host(screen_id=7,target_size=(1280,900),capture_center=(640,450),watchdog=2)
    stream=FakeSession()
    host.start(acknowledged=True,input_state_clear=True)
    try:
        with X11Backend() as sender:
            mapping=sender.keymap()
            sender.motion(650,455)
            sender.key_hid(mapping,4,True); sender.key_hid(mapping,4,False)
            end=time.monotonic()+2
            while time.monotonic()<end:
                host.pump(stream)
                if sum(1 for kind,fields in stream.events if kind=='key' and fields['usage']==4)==2 and any(kind=='move' for kind,_ in stream.events): break
                time.sleep(.01)
            else: raise AssertionError('X11 host did not capture synthetic motion and key events')
    finally:host.restore_local()
    print('PASS: isolated X11 host capture, pointer recenter, bounded motion queue and key releases')
    host=X11Host(screen_id=7,target_size=(1280,900),capture_center=(640,450),watchdog=2)
    host.start(acknowledged=True,input_state_clear=True)
    try:
        with X11Backend() as sender:
            mapping=sender.keymap()
            for usage in (0xE1,0xE0,0xE2,0x29):sender.key_hid(mapping,usage,True)
            end=time.monotonic()+2
            while host.thread.is_alive() and time.monotonic()<end:time.sleep(.01)
            if host.thread.is_alive() or not host.error:raise AssertionError('emergency chord did not release capture')
            for usage in (0x29,0xE2,0xE0,0xE1):sender.key_hid(mapping,usage,False)
        with X11Backend() as probe:probe.grab();probe.ungrab()
    finally:host.restore_local()
    print('PASS: isolated Ctrl+Alt+Shift+Esc emergency release')
    os.environ['HOP_VIRTUAL_X_SERVER']='1'
    from xkb_caps_virtual_test import run as run_caps
    run_caps()
    os.environ["HOP_VIRTUAL_X_SERVER"] = "1"
    from clipboard_virtual_test import run
    run()
    from panel_virtual_test import run as run_panel
    run_panel()
    from layout_virtual_test import run as run_layout
    run_layout()
    # This is intentionally after GTK initialization: the fresh capture
    # subprocess must not depend on XInitThreads ordering in its parent.
    from hop_linux.x11_host_process import X11HostProcess
    from types import SimpleNamespace
    stream=FakeSession();stream.role='host';stream.phase='remote';stream.transport=SimpleNamespace(authenticated=True)
    child_capture=X11HostProcess(screen_id=7,target_size=(1280,900),capture_center=(640,450))
    stream.sink=child_capture
    child_capture.start(stream)
    try:
        with X11Backend() as sender:
            mapping=sender.keymap();sender.key_hid(mapping,4,True);sender.key_hid(mapping,4,False)
            end=time.monotonic()+2
            while time.monotonic()<end:
                child_capture.pump(stream)
                if sum(kind=='key' and fields['usage']==4 for kind,fields in stream.events)==2: break
                time.sleep(.01)
            else: raise AssertionError('subprocess failed to capture synthetic key')
    finally:child_capture.restore_local()
    with X11Backend() as sender:
        mapping=sender.keymap();sender.key_hid(mapping,4,True)
        held=X11HostProcess(screen_id=7,target_size=(1280,900),capture_center=(640,450));stream.sink=held
        try:
            try: held.start(stream)
            except RuntimeError: pass
            else: raise AssertionError('capture accepted physically held key')
        finally:
            held.restore_local();sender.key_hid(mapping,4,False)
        sender.button(8,True)
        held=X11HostProcess(screen_id=7,target_size=(1280,900),capture_center=(640,450));stream.sink=held
        try:
            try: held.start(stream)
            except RuntimeError: pass
            else: raise AssertionError('capture accepted physically held side button')
        finally:
            held.restore_local();sender.button(8,False)
    stalled=X11HostProcess(screen_id=7,target_size=(1280,900),capture_center=(640,450));stream.sink=stalled
    stalled.start(stream)
    try:
        end=time.monotonic()+3
        while stalled.process.poll() is None and time.monotonic()<end: time.sleep(.02)
        if stalled.process.poll() is None: raise AssertionError('stalled host helper did not exit')
        with X11Backend() as probe: probe.grab();probe.ungrab()
    finally: stalled.restore_local()
    print('PASS: capture subprocess starts after GTK, and rejects held X11 keys and XI2 side buttons')
    from session_xvfb_test import run as run_session
    run_session()
    from session_xvfb_test import run_host
    run_host()
    from session_xvfb_test import run_clipboard
    run_clipboard()
    from pair_ui_virtual_test import run as run_pair_ui
    run_pair_ui()
    from pair_ui_virtual_test import run_two_windows
    run_two_windows()
    from pair_ui_virtual_test import run_multi_window
    run_multi_window()
    from pair_ui_virtual_test import run_receiver_dial
    run_receiver_dial()
    from role_ui_virtual_test import run as run_role_ui
    run_role_ui()
    from clipboard_app_virtual_test import run as run_clipboard_app
    run_clipboard_app(private_x_server)
