# Hop for Linux

The `hop-input_2.0.0~rc1_amd64.deb` candidate targets Ubuntu 22.04 amd64 in
an X11 desktop session. Hop can host or receive keyboard and pointer input on
an encrypted local connection, arrange multiple peers, and share UTF-8 text or
PNG with the active device when both sides grant permission. The candidate has
isolated Xvfb and WSL coverage. Physical-device input, real LAN discovery,
display scaling, Wayland and cross-platform compatibility await validation.

## Install and use

On a compatible Linux desktop, install the local package and launch **Hop**
from the application menu or run `hop-input`:

```sh
sudo apt install ./hop-input_2.0.0~rc1_amd64.deb
```

- Use an X11 login session and an unlocked Secret Service collection for device
  certificates. Avahi enables local discovery. You can also enter the
  receiver's LAN address and TCP port (24891 by default). A Linux receiver can
  also dial a waiting host. To connect from HarmonyOS or another receiver that
  initiates the connection, select **Wait for a receiver** in the Linux host
  window, then choose or enter the host's IPv4 address on the receiver.
- Select the host and receiver roles. Compare both full SHA-256 fingerprints
  on the devices before confirming the connection. Add further devices in the
  host workspace and use **Arrange connected devices** to set their positions.
- Release the mouse buttons and keys and turn off Caps Lock before starting
  remote control. Press **Ctrl+Alt+Shift+Esc** to restore local input if needed.
  Edge crossing must be enabled in the host workspace. On X11 desktops with a
  compatible notification area, the Hop icon displays connection status and
  offers pause/return and resume actions. [GTK notification-area support](https://docs.gtk.org/gtk3/class.StatusIcon.html)
- Enable clipboard permission in each connection window before connecting.
  Both ends must opt in and the receiver must be under active control. Uncheck
  the permission to stop synchronization. Reconnect to grant permission again.
  Files and file-list selections are excluded. Text is limited to 2 MiB and
  PNG to 16 MiB and 16,777,216 pixels. The last synchronized value can remain
  the local X11 selection until another program copies or its owner exits.
- `hop-input --check` inventories environment and libraries. Remove the package
  with `sudo apt remove hop-input`. Credentials and layouts belong to your
  desktop session and can persist across a reinstall.

## Current components

- `hop_linux.x11.X11Backend` opens an X11 display only when explicitly created.
  `grab()` is explicit; it rolls back a successful keyboard grab if pointer grab
  fails. `ungrab()` releases pointer and keyboard first. Context manager / close
  attempts release of known injected keys and buttons and reports failures.
  Mouse movement uses XTest absolute coordinates; common buttons 1/2/3/8/9 and
  discrete wheel actions on both axes are exposed. `keymap.py` translates server
  XKB physical names to USB HID usage IDs without assuming an evdev offset;
  ambiguous and unknown names are rejected, and pressed keys retain their
  original keycode for release if the mapping changes. Names are read from the
  X server; applications must release held keys before refreshing the mapping.
- `clipboard.py` implements bounded selection-transfer policy, and
  `clipboard_x11.py` implements a first Xlib event driver. The driver must run
  in a dedicated helper process because Xlib's error handler is process-global.
  It accepts UTF-8 and structurally checked PNG, caps text at
  2 MiB and PNG at 16 MiB/4096² pixels, gates remote operations on active peer
  and epoch, rejects stale callbacks, and bounds/ages ICCCM INCR transfers. It
  does not decode PNG. Its event pump owns a separate X window and must be
  called regularly. The integration must watch local owner changes, preserve
  already accepted INCR snapshots on SelectionClear, stop network sync when
  focus changes, and explicitly define ownership on shutdown
  (the selection normally disappears when its connection closes unless a
  clipboard manager saved it). Never include clipboard contents in diagnostics.
- `panel.py` provides a native GTK3 bilingual configuration **preview** with
  unavailable actions disabled. It has no session controller or tray yet.
  Chinese text requires an installed CJK font; test-only renders used Windows
  system fonts without copying or distributing those files. Run explicitly with
  `PYTHONPATH=apps/linux python3 -B -m hop_linux --preview-ui` on a GTK3 desktop.
- `layout.py`, `editor.py` and `layout_panel.py` provide a simulated GTK3
  multi-device layout editor with expandable displays, edge snapping, overlap
  rejection and a disconnected-layout warning. Run the development preview
  with `PYTHONPATH=apps/linux python3 -B -m hop_linux --layout-preview`.
  It contains sample devices only and does not discover or control computers.
  The preview does not save unless its integrating caller explicitly provides
  app-owned layout and profile paths; it is not a pairing or session UI.
- `profiles.py` provides per-peer settings keyed by verified SHA-256 certificate
  fingerprint with locked, atomic persistence. Separate movement sensitivity,
  horizontal/vertical scroll multipliers and inversion are supported. These are
  software transforms only: they do not change the mouse's hardware DPI or
  global operating-system settings. The development pairing UI applies these
  settings to its one authenticated target.
- `tls.py` adapts the shared experimental TLS C ABI to a nonblocking socket.
  Constructing a peer pin requires a certificate and a separately verified
  SHA-256 fingerprint; no first-use trust, automatic pairing, or plaintext
  fallback is implemented. Device identities and pins can be stored with Secret
  Service; the host workspace associates each connection with its verified pin.
- `codec.py` and `session.py` provide an experimental v2 codec and serialized
  authenticated connection state machine, including HELLO, screen declaration,
  acknowledged focus changes, input framing and failure cleanup. Native input
  and credential adapters, discovery and clipboard sessions are wired into the
  GTK connection controllers. The owner loop sends interactive input ahead of
  the next clipboard chunk and rejects nonzero held state on focus entry.
- `pairing.py` separates explicit comparison of both full certificate
  fingerprints from persistence: a peer is stored only after matching the
  authenticated connection and receiving both v2 pairing confirmations.
  Experimental GTK host/receiver pairing windows and Avahi discovery are
  connected; they have only been tested using isolated services and synthetic
  peers, not real cross-platform devices or a real Avahi daemon.
  `secret_service.py` can use libsecret and an unlocked Secret Service
  collection for identity and peer records; unavailable or locked services
  fail without a plaintext replacement. The pairing window can explicitly list
  stored peer fingerprints and select one for reconnection. Last authenticated
  connections may supply a saved, untrusted address hint; a changed address can
  still be entered manually and every connection rechecks the pinned certificate.
  Calls can block and belong on a worker
  thread. Python/GI may retain memory copies of key material.
- `x11_host_process.py` starts a separate, explicit X11 capture process only
  after focus acknowledgment. It initializes Xlib threading in that fresh
  process and rejects capture when the X server reports held keys or XI2 mouse
  buttons. An independent watchdog releases the X grab on stalled control.
  These checks observe logical X server state and cannot prove physical-device
  timing; no real-device held-input handoff has been validated. The development
  pairing UI starts it after a manually requested or explicitly enabled edge
  transition has been acknowledged.
- `shortcut.py` binds the shared stateful shortcut C ABI. When a caller supplies
  the runtime and configures a translator per focus, the host input controller
  translates keys and flushes pending modifiers before pointer events. The
  Debian candidate includes the shortcut, TLS and clipboard runtimes.
- Experimental pairing windows require an absolute path to a locally built,
  trusted TLS shared library, Python D-Bus/GI, Avahi, and an unlocked Secret
  Service. Run `PYTHONPATH=apps/linux python3 -B -m hop_linux --pairing-ui
  --tls-library /absolute/path/libhop_tls_runtime.so` for the host or replace
  `--pairing-ui` with `--receive-pairing-ui` for the receiver. The receiver
  does not accept input unless `--enable-receiver-input` is also explicitly
  provided. Host control additionally requires `--shortcut-library` with an
  absolute path; it starts only after pairing, screen discovery and an explicit
  click or after the user explicitly enables experimental edge observation.
  The read-only sampler detects movement from inside a monitor to its boundary;
  it does not detect further pushes while already at the boundary. Ctrl+Alt+
  Shift+Esc is the emergency local-input restore shortcut. The GTK workspace
  opens one connection window per peer and shares layout and focus authority.

  Build the bundled libraries with `native/CMakeLists.txt` and independently
  verified mbedTLS 3.6.7 sources. Use `packaging/build_deb.py --help` to package
  the three ELF libraries, their SHA-256 digests and the upstream Apache 2.0
  license. Use a fresh output directory on a Linux filesystem.
- `multi_session.py` coordinates a single active focus over multiple previously
  authenticated connections, with release acknowledgment and local revocation
  barriers before the next ENTER. It has simulated and two-pinned-TLS-connection
  tests but is not connected to the device list or layout UI.
- `clipboard_initial.py` drives a host's initial text or PNG snapshot after
  both sides are ready. The receiver records its local clipboard version on
  ENTER, discards a conflicting initial snapshot and waits for validation and
  application to finish before returning updates. The GTK connection
  controllers start the X11 actor after the user grants permission. The actor
  rejects stale owner versions and acknowledges revocation before the session
  finishes releasing focus.
- The backend deliberately rejects Wayland sessions rather than silently claim
  that XWayland access controls the whole desktop. Wayland capture and injection
  still need XDG InputCapture / RemoteDesktop portals and libei, negotiated and
  tested with each compositor. Runtime libraries being present do not prove a
  compositor grants permissions or implements the required capabilities.
- X11 event grab semantics, key repeat/lock state, display geometry/scaling,
  relative cursor motion, safe local/remote held-key handoff, compositor and
  accessibility interactions, and behavior on display failure or process death
  remain to be implemented and verified on an actual desktop. XTest can affect
  current server input state; the caller needs a safe backup device and explicit
  user consent before any live capture or injection. X11 itself does not
  protect clients from a malicious client on the same server.

The read-only diagnostic checks environment and library presence without opening
an X11 display, requesting portal permission, capturing input, injecting input,
or connecting to a peer:

```sh
PYTHONPATH=apps/linux python3 -B -m hop_linux --check
```

## Test

```sh
PYTHONPATH=apps/linux python3 -B -m unittest discover -s apps/linux/tests -v
```

The socket TLS tests are skipped unless both `HOP_TEST_TLS_LIBRARY` (absolute
path to a locally built shared library) and `HOP_TEST_TLS_FIXTURE_DIR` (a
private directory containing disposable `a`, `b`, and `bad` test certificate
and key PEM files) are supplied. They use loopback only; never point this test
at production device credentials.

Unit tests use fake or isolated X11 APIs; they do not operate on the real desktop.
On Ubuntu 22.04 WSL x86_64, tests and dynamic X11/XTest library binding passed without
opening the desktop; a later read-only XKB-name query on WSLg Xwayland mapped
118 key names with all eight left/right modifier positions. No input was
captured or injected. This is not X11 or Wayland real-device acceptance. The
userland Zig toolchain used for shared C++ core compilation was downloaded from
its official release, matched the upstream SHA-256, and unpacked into a private
WSL temporary directory without changing the system environment.

An isolated Xvfb server was additionally run inside a private Linux user,
mount, and network namespace with a private `/tmp`, an ephemeral Xauthority
cookie and TCP disabled. X11 input grab, XKB/HID mapping, XTest movement,
buttons and both scroll axes passed there. Two processes exchanged INCR text
and PNG through X11 CLIPBOARD selections, and GTK3 bilingual content was
rendered offscreen. The application test uses three separately authenticated
private displays to verify initial snapshot, target-local conflicts, PNG,
Unicode and empty text, file-list exclusion, focus switching, revocation and
actor death. It uses disposable certificates, synthetic selections and a
private network namespace.

Primary API references: [Xlib](https://www.x.org/releases/X11R7.6/doc/libX11/specs/libX11/libX11.html),
[XTest](https://www.x.org/archive/X11R6.8.0/PDF/xtestlib.pdf),
[XDG InputCapture](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.InputCapture.html),
[XDG RemoteDesktop](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html).
