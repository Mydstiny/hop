# Hop v2 portable core (work in progress)

This isolated C++17 library contains layout geometry, a focus-transfer policy,
and small input-setting transformations. It is **not connected** to the v1
Mac or HarmonyOS applications, a network protocol, or physical input devices.
It is not an installable product and does not establish real-device behavior.

- `Layout` validates bounded, non-overlapping half-open rectangles and resolves
  edge crossings. Screen IDs must be unique across all devices. An uncovered
  edge, a gap, and a corner-only contact do not cross. The caller decides how
  physical screens map to layout coordinates and whether a target is available.
- `Focus` is a local policy state machine. Forwarding is forbidden during a
  transfer, and a remote-to-remote transfer requires a matching release ack
  before the new enter ack. Generations reject stale acknowledgements. Adapters
  must supply authentication, timeouts, actual release, local restoration and
  remote cleanup; the class cannot provide those behaviors by itself.
- `input_policy` handles bounded per-target movement and wheel settings plus a
  small, explicitly enumerated set of common shortcut chord transformations.
  It is not a complete keyboard map or event sequencer. Native adapters must
  separately manage modifier down/up order, layout/IME and physical-key state.
  In particular, redo keys and shifted app-specific shortcuts need further
  platform and application testing; no universal mapping is assumed.

Run on macOS or Unix with a C++17 compiler:

```sh
sh core/tests/run.sh
```

Or build on macOS, Linux, and Windows with CMake 3.16+:

```sh
cmake -S core -B build/core -DBUILD_TESTING=ON
cmake --build build/core --config Release
ctest --test-dir build/core -C Release --output-on-failure
```

The GitHub Actions matrix is configuration only until it runs on GitHub. The
HarmonyOS ARM64 toolchain can cross-compile the static library, which does not
show that it is integrated into or running on a device. Existing v1 code and
its production build remain independent.

## Experimental authenticated transport

`HOP_ENABLE_EXPERIMENTAL_TLS=ON` builds a separate TLS 1.3 channel and a C ABI
on top of Mbed TLS 3.6.7. The dependency is downloaded only when this option is
enabled and is pinned to its upstream SHA-256. Mbed TLS is available under the
Apache-2.0 license; see its upstream distribution for the license and notices.
The ordinary core build remains dependency-free.

Each connection requires its own certificate and private key plus the exact
certificate of the previously verified peer. Both sides require certificate
verification and pin the peer's exact certificate before application data can
be read or written; a missing or changed pin fails closed. Certificate exchange,
user verification, secure identity persistence, socket timeouts, cancellation,
session revocation, queue limits and application protocol framing are **not yet
implemented**. In particular this code does not make the currently running v1
connection encrypted and does not implement clipboard synchronization. It has
not had an independent security review. Callers must protect and clear their own
private-key buffers, and provide nonblocking, bounded transport callbacks.

The optional test uses freshly generated disposable certificates and in-memory
fragmented I/O; it does not use the user's identities or network:

```sh
HOP_MBEDTLS_SOURCE_DIR=/path/to/mbedtls-3.6.7 sh core/tests/run_tls.sh
```

The HarmonyOS SDK injects an unused `--gcc-toolchain` argument which upstream
Mbed TLS treats as an error with its default fatal-warning setting. For that
toolchain only, configure `-DMBEDTLS_FATAL_WARNINGS=OFF`; warnings remain visible.

## Experimental bounded framing

`frame.h` specifies a 28-byte network-byte-order HOP2 header: ASCII `HOP2`,
version (2), kind (1 byte), reserved zero flags (2 bytes), payload length
(4 bytes), sequence (8 bytes), and focus epoch (8 bytes). Kinds 1–3 have a
4 KiB payload ceiling; kind 4 is a nonempty clipboard chunk up to 64 KiB.
Zero epochs/sequences, unknown kinds, reserved flags, oversized/truncated
frames and repeated or decreasing sequence numbers are rejected. The decoder
holds at most one bounded payload; it does not authenticate any bytes and
must be fed only from an already authenticated TLS connection. Epoch validity,
message schemas, clipboard assembly limits and authorization remain the
caller's responsibilities. Each new TLS session requires a fresh decoder.

`outbound_queue.h` gives control/input priority between clipboard chunks and
bounds queued clipboard bytes. Input and control preserve their order with
each other; clipboard control and chunks preserve theirs. A frame already in
flight cannot be preempted, and implementations must enforce socket deadlines.
An enqueue failure must produce backpressure or close safely, never silently
lose key-up events. Focus changes must discard queued clipboard frames, while
the receiver independently rejects stale epochs. Neither queue is connected
to any product transport yet.

The experimental POSIX socket wrapper (`posix_tls_socket.h`) owns an already
connected IPv4/IPv6 TCP socket, switches it to nonblocking mode and prevents
SIGPIPE where the system provides the appropriate socket option/flag. It
requires the caller to supply an independently verified peer certificate,
handles the TLS library's same-buffer write retry requirement, and never falls
back to plaintext. A local TCP loopback test covers both pins, pre-auth denial,
closure and write backpressure; the wrapper cross-compiles for HarmonyOS ARM64.
It has not been integrated into either application. Socket lifecycle, deadlines,
identity storage and peer verification remain application responsibilities.

`shortcut_translator.h` is an experimental, single-owner keyboard state
machine. It defers the source Command/Control modifier until a common shortcut
can be distinguished from a standalone modifier; pointer events must first
flush pending modifiers, and focus loss must release all output keys. It keeps
left/right modifiers and Caps Lock state. The C ABI is in
`shortcut_translator_c.h`. An invalid state is terminal and the caller must
stop forwarding and restore the local device. Chords outside its explicit
small set remain physical, and input methods, applications, system launchers,
real hardware timing, and every platform combination still require testing.
It is not connected to the current Mac/Harmony v1.
