# Hop for Windows

The Windows x64 release candidate uses the TLS-enabled tray. It includes
manual-IP pairing, explicit host and receiver controls, experimental clipboard
sync and DNS-SD discovery. The default build without TLS is a disconnected tray.
Per-device pointer policies use shared `core/` math. Real desktop and
cross-device acceptance and autostart are open.

`hop_windows_tray` starts disconnected and never installs hooks or captures
input on startup. With `HOP_WINDOWS_EXPERIMENTAL_TLS=ON`, its development UI
can start a listener, browse IPv4 LAN candidates or connect by numeric address, compare full certificate
fingerprints, confirm again over pinned TLS, choose host or receiver, and
explicitly enable input. The default build keeps these controls absent. These
paths have isolated transport and fake-device tests. The GUI, real hooks,
real clipboard and physical-device operation await acceptance. A Windows
host can explicitly add up to eight authenticated devices. Use **Pause** to
restore local control and **Switch target device** to select an established
peer. The menu displays the device name and a short fingerprint. A saved
peer can reconnect to an explicitly enabled listener using its existing pin.

The layout window places the local displays and all established peers on
one workspace. Explicit edge monitoring can choose a peer from the layout,
cross directly to another peer or monitor, and return to the local desktop.
Each transition restores local control before waiting for the prior release
acknowledgement and clipboard drain, then starts the next capture after its
entry acknowledgement. The manual target menu is also available while the
host is idle.

An explicit layout edit saves one arrangement per set of authenticated
peers. Hop restores positions and expansion only when the screen IDs,
native positions and dimensions match the current displays. An edit after
a topology change can replace the older arrangement for that peer set.
English and Chinese follow the system or `--lang=en` / `--lang=zh`.

The v2 discovery service is `_hop2._tcp.local`, advertises only `v=2` while
an explicit listener is active, and treats discovered names and addresses as
untrusted hints. The UI still requires the normal fingerprint comparison and
TLS confirmation. Automated checks synthesize DNS results without browsing the
real network. Interoperability, IPv6 discovery, service removal and hostname
resolution on real Windows networks remain unverified.

- `InputSink` constructs Windows `SendInput` calls for physical scan codes,
  five mouse buttons, absolute virtual-desktop mouse position and both wheel
  axes. It tracks successfully injected downs, releases all tracked inputs on
  request, retains failed releases for retry and never emits unmatched ups.
  Its injected callable allows tests to inspect the actual `INPUT` values
  **without calling** Windows input services. Real `SendInput` is subject to
  Windows UIPI, session and desktop restrictions; a return count is not proof
  the target application received or accepted an event. Errors distinguish
  malformed/unsupported sequences, unmatched releases, duplicate buttons and
  system rejections; they do not claim that UIPI was the cause.
- `keymap` maps 127 physical HID keyboard-page usages to Windows set-1 scan
  identities and back, including separate left/right modifiers, E0 navigation
  and keypad keys. E1 Pause is represented separately from NumLock; injection
  of that E1 sequence is explicitly unsupported pending real-device evidence.
  Legacy E0 45 NumLock is accepted as an input alias. Usage 0x32 shares a scan
  with 0x31 and cannot be unambiguously reconstructed, so it is rejected.
  Power and release-only locale keys are also excluded. The physical table is
  based on [Microsoft's scan-code table](https://learn.microsoft.com/windows/win32/inputdev/about-keyboard-input#scan-codes).
- `InputOwnership` is a pure single-threaded capture policy with fixed-size
  state; it refuses remote activation while a local input is held and drains
  remotely owned input tails after deactivation. Unsupported physical keys are
  tracked too. If a remote mouse button remains held, pointer movement is also
  suppressed while draining, because a low-level `WM_MOUSEMOVE` does not tell
  which physical button caused it. Tests do not query or modify device state.
- `InputCapture` is an **experimental, unintegrated** Win32 low-level hook
  adapter. It runs a dedicated message loop, ignores injected events, uses a
  fixed-size single-producer/single-consumer queue, and fails out of remote
  mode on queue overflow. Ctrl+Alt+Esc signals an emergency and deactivates
  capture. Activation/deactivation are asynchronous. Stop is refused while
  capture is active or remote-owned releases remain to drain. Destruction and
  process death cannot guarantee a final physical up will be observed.
- `ClipboardSync` and the optional Win32 backend handle plain UTF-8 text and
  registered `PNG` clipboard format, gated by explicit enablement, a trusted
  active peer, focus generation and a sampled baseline sequence. Generation,
  sequence and written-change suppression prevent old-focus replay and local
  write echoes; a failed/unknown-sequence write pauses synchronization. File
  objects (`CF_HDROP`, shell file descriptors and URI-list formats) are rejected
  before content access; paths carried as ordinary text remain valid text. Payloads have byte/dimension limits; PNG chunk
  structure and CRCs are checked; APNG and iCCP/zTXt/iTXt metadata are rejected
  to avoid inflating unbounded metadata. WIC converts PNG to a
  top-down BGRA DIBV5 and converts supported DIB/DIBV5 back to PNG in memory.
  On write both clipboard formats are prepared before opening/emptying the
  clipboard, checks the sequence captured when the transfer began before changing it, and Windows owns each successfully transferred handle. Supported
  DIB input is 24/32-bit BI_RGB or standard 32-bit BI_BITFIELDS; palettes,
  alternate channel masks and embedded/linked color profiles are rejected.
  Color-profile fidelity and actual target-application paste compatibility are
  not yet verified. The experimental runtime activates clipboard operations
  only after the user explicitly enables them and a trusted focus is active.
- `TargetPointerSettings` selects independent movement and horizontal/vertical
  scrolling multipliers and reversals by verified device certificate
  fingerprint. A versioned binary record can be saved under the current user's
  registry; the hot input path uses cached settings and does no registry I/O.
  Each target also has a maximum movement forwarding rate (default 240 Hz);
  moves may be coalesced, while button/key/scroll ordering is retained. This
  adjusts **software sensitivity**, not physical mouse hardware DPI.
  The experimental tray applies these settings to its connected trusted peer.
  Automated tests use a fake store and never access the registry.
- `Session` is an experimental serialized v2 session state machine using the
  shared payload validator, frame decoder, and bounded outbound queue. It
  supports basic HELLO, screen announcement, focus ACK/release, basic input,
  heartbeat, and epoch checks. `SessionPump` connects this state machine to the pinned TLS
  socket on a worker. It negotiates basic input, horizontal scroll, and side
  buttons. An explicitly
  enabled experimental clipboard path uses the shared bounded assembler and
  carries a local sequence baseline to the owner callback, which must validate
  content, recheck focus and conditionally write off the session worker. It is
  disabled by default; the experimental runtime connects it to the clipboard
  worker after explicit enablement. After both peers are ready, the host queues
  one initial text/PNG snapshot or an explicit skip; the receiver preserves a
  clipboard change made since ENTER and waits for the local worker before
  sending its own changes. The application remains unfinished.
- `CandidateExchange`, `PairingReview`, `PeerSelector`, and `ConnectionPath`
  implement bounded HPOP candidate exchange, explicit fingerprint approval,
  an HPSL lookup hint, pinned TLS and in-session mutual confirmation. A
  credential manager backend stores the local identity and confirmed peer
  certificates. Candidate names are untrusted, and pending approval expires.
  Native tests use loopback and an in-memory credential backend; no real
  credential manager, discovery service, physical input or interactive UI was
  touched. The experimental tray application connects these components; its
  interactive behavior has not yet been validated.
- The input adapters now include host-side capture/translation workers and a
  receiver session adapter. Real hook startup and local cursor recentering need
  an explicit product action, after focus acknowledgement. Multi-monitor DPI,
  hook lifetime, emergency recovery and cursor behavior remain unverified on
  an interactive desktop. Do not enable this experimentally in normal use.
- `TlsSocket`, built only with `HOP_WINDOWS_EXPERIMENTAL_TLS`, wraps the shared
  TLS C ABI around an already-connected nonblocking Winsock socket. An
  independently verified peer certificate is required for every connection;
  no trust-on-first-use or plaintext fallback is provided. The experimental
  tray connects this pump to explicit pairing and identity verification; the
  resulting end-to-end behavior still requires interactive device testing.

Important integration requirements:

1. Before transmitting events, require authenticated encrypted v2 session and
   the correct acknowledged input-focus generation. Do not use plaintext v1
   as a fallback.
2. Stop forwarding synchronously on fault, overflow, emergency or focus loss;
   discard queued old-generation events, send release to the receiver, restore
   local input and keep the hook draining as required. There is no remote
   release or session management inside this adapter.
3. Passive move events are available for edge detection; the consumer must
   drain them before activation. Actual mouse delta/warp behavior, multiple
   displays, DPI and hardware effects need testing on a real interactive
   Windows desktop. SSH Session 0 is not evidence of these behaviors.
4. Low-level hooks can be silently removed on timeout; secure desktop, elevated
   targets, unexpected injected input, device disconnection and process death
   have platform-specific limits. Do not call this ready for daily use until
   hook lifecycle and the input ownership/restore paths have been tested.
5. Test the connected HID translation path with actual keyboards.
   `CaptureEvent` carries the scan identity, E0/E1 distinction,
   virtual-key hint, down/up and physical repeat; the sink accepts repeated
   downs and tracks the corresponding release. Windows exposes Pause via
   legacy VK/scan conventions, which still need real-hardware testing. E1/Pause
   injection, special combinations, layout/IME, modifier sequencing and
   some semantic shortcuts require further work. Virtual-desktop coordinate
   conversion exists but remains unverified on real multi-monitor setups.

CMake on a Windows development machine with a C++17 compiler:

```powershell
cmake -S apps/windows -B build/windows -DBUILD_TESTING=ON
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release --output-on-failure
```

For the isolated TLS socket adapter, enable the option and provide the pinned
mbedTLS 3.6.7 source. Its tests use generated identities and loopback only:

```powershell
cmake -S apps/windows -B build/windows-tls -DBUILD_TESTING=ON -DHOP_WINDOWS_EXPERIMENTAL_TLS=ON -DHOP_MBEDTLS_SOURCE_DIR=C:/path/to/mbedtls-3.6.7
cmake --build build/windows-tls --config Release
ctest --test-dir build/windows-tls -C Release --output-on-failure
```

On 2026-11-09 this snapshot built natively on the user's Windows machine
using GNU 13.2/CMake/Ninja in an isolated user-owned test directory; CTest
passed all 4 tests: 94 fake-injection checks, 31 input-ownership checks,
150 mapping checks and 12 UI-state checks. The clipboard follow-up passed
all 7 CTest targets on the Windows machine, including fake-backend clipboard,
in-memory DIB and real WIC PNG/DIBV5 round-trip tests; no actual clipboard
reads, writes or listeners were used. An import-table inspection of the
cross-compiled tray binary found `Shell_NotifyIconW` but no `SendInput` or
`SetWindowsHookEx` imports. The
capture and sink sources also cross-compiled as Windows ARM64 objects; they
have not run on Windows ARM64. These are fake-injection and pure-policy tests only: they
never call `SendInput`, install hooks, request input permission or test a
physical device. A failed attempt at streaming an executable over SSH timed out
and its separate temporary directory was left in place.

On 2026-11-10 an isolated native Windows build passed 27 CTest targets,
including shared v2 golden vectors, session lifecycle tests and pinned TLS
loopback with synthetic input and clipboard data. The session tests cover
Windows-to-simulated-Mac common shortcut translation including Caps Lock,
focus epochs, release/timeout, clipboard chunking and conflict guards. Tests
use fake hooks and clipboard backends. The suite also covers HLS1
persistence with fake storage and rejects changed local or peer geometry
before starting edge monitoring. A two-peer test exercises automatic
local-to-peer and peer-to-peer crossing. A blocked old clipboard write
prevents the next entry after local input has resumed and the old peer has
released. The test also verifies that a standby disconnect leaves an
explicitly selected capture active.

The Windows test-only `hop_windows_interop_tool` exercises the same pairing
transport as the tray using temporary identities, memory-backed trust and
fake input. Windows and Mac passed in both roles through loopback TCP
forwards, including Ctrl+C and Command+C translation. Windows host to
HarmonyOS fake SDK and both Windows/Linux role combinations also passed
isolated adapter tests. These checks covered HPOP, HPSL, pinned TLS, screen
announcement, focus, input and release. Interactive and physical-device
acceptance remains open. See [installer/README.md](installer/README.md) for
the x64 candidate and installation checks.
