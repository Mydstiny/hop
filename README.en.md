# Hop — Mac → HarmonyOS PC keyboard and mouse sharing

[中文](<README.md>) · [Plan and evidence](<PLAN.md>) · [Hop Protocol v1](<docs/PROTOCOL.md>) · [Real-device acceptance](<docs/ACCEPTANCE.md>)

The keyboard and mouse stay physically attached to **one Mac**. Push the pointer across a configured left/right edge to control **one HarmonyOS PC**; the keyboard follows. Push back across the opposite edge to return. Nothing more.

## Status — not yet hardware-accepted

This repository contains native applications and test tooling. A passing build, localhost handshake or fake injection test is **not** proof that physical input, system authorization, tray residency or two-device crossing works. Exact commands, results and outstanding gates are kept in the [plan](<PLAN.md>). Complete the unchecked [acceptance checklist](<docs/ACCEPTANCE.md>) before calling the product ready for daily use.

- **Mac:** Swift/AppKit, macOS14+, arm64 and Intel, LSUIElement menu-bar popover only. Active CGEventTap capture; a single idempotent restore-first safety path; Keychain pairing token.
- **HarmonyOS PC:** HarmonyOS6/API20+ minimum, Stage ArkTS and NAPI C++. Receive/inject only, system injection consent, native statusbar popup, legitimate long-running task and persistent notification. No global input capture or accessibility workaround.
- **Wire:** independently specified Hop v1, one native TCP stream, four-byte big-endian length prefix, fixed128-byte binary body, ten top-level message types. HELLO_ACK and KEY_DOWN/UP are preserved as subtype/action semantics. No JSON wire format or third-party networking library.

### Verified in this run

- Native universal Mac build passed: arm64 + x86_64, minimum14.0; complete Xcode build remains unverified.
- DevEco native/ArkTS/HAP packaging passed; output is **unsigned**, not an installed signed release.
- 40 protocol Python tests,644 Swift checks plus8 TCP scenarios,354 C++ checks, and two actual Swift↔C++ left/right input/return scenarios passed (recording fake SDK only).
- The static Xcode checker resolves40 objects/53 links, all10 Swift sources and resource/scheme wiring;17 positive/negative checks passed. This is not an xcodebuild run.
- Physical input, tray/background behavior and two-device runtime remain unverified. Strict M0–M4 gates are not all marked complete.

Run `python3 tools/verify.py` for the combined regression. See the [machine-readable report](<critique/build-test.json>) and [milestone evidence](<PLAN.md>).

Read-only prerequisites: `python3 tools/device_preflight.py --target YOUR_DEVICE_TARGET --require-prerequisites`; see the [current audit](<critique/device-preflight.json>). Its25 safety tests pass, but actual prerequisites do NOT: full Xcode and project signing are absent, and the official SDK verifier confirms no HAP signing block. Default exit0 means report collected only; the strict flag returns2 for missing prerequisites. It never installs/launches apps, requests authorization, reads UDIDs/private keys, or injects input; multiple targets require explicit selection. Verifier outputs stay in git-ignored private artifacts. A valid signature alone still does not prove app/device-profile eligibility or physical runtime acceptance.

## Security: trusted LAN only

**v1 uses a persistent random token for authentication and cleartext transport. There is no TLS, confidentiality or active-MITM protection.** An attacker on the LAN may observe/modify input or steal the token. A matching six-digit code and two confirmations do not solve that transport limitation. Never expose the TCP port to the Internet or use this on untrusted/public Wi-Fi.

Both devices must explicitly confirm the same six-digit pairing code. Mac stores the token in Keychain, HarmonyOS in Preferences. Emergency on Mac: **Control+Option+Command+Escape**. Have fallback physical local input during first acceptance. OS cursor cleanup after process crash/SIGKILL must be measured separately; a finally block cannot run after process death.

## Strict scope

Included: keyboard/mouse input, left/right edge crossing, LAN pairing, one Mac host and one HarmonyOS PC receiver.

Excluded: clipboard, files/cross-computer drag-and-drop, display extension, phones/tablets, HarmonyOS as host, accounts/cloud/public networking, app-store publishing, multiple peers/topology. Dragging a window or selecting text entirely within the remote PC is ordinary supported mouse input, not cross-computer file transfer.

Hop is not Deskflow-compatible and does not incorporate its protocol, GPL server or source code. The older research attachment's Deskflow/TLS/clipboard proposal is superseded by the current strict requirements.

## Build and test

Products use native OS APIs and standard libraries only. SDK/build tools naturally have their own vendor-supplied build dependencies; these are not bundled third-party product libraries.

### Protocol and fake peer — Python3.10+, no pip dependencies

```sh
python3 tools/generate_keymap.py --check
python3 -m unittest discover -s tools/mock-peer -v
```

The [canonical mapping](<protocol/keymap.json>) is the single source for generated Mac/native resources and the [readable key table](<docs/KEYMAP.md>). Shared [golden vectors](<protocol/golden-vectors.json>) verify binary agreement across languages. Tests cover framing, partial reads/concatenation, malformed frames, pairing and persistence barriers, stale epochs, release state, heartbeat deadlines and latest-only movement coalescing. They do not capture or inject physical input.

Two terminals can run mock peers:

```sh
python3 tools/mock-peer/hop.py server
python3 tools/mock-peer/hop.py client
```

The default is localhost and interactive `yes` confirmation on each mock panel. Automated test-only example:

```sh
python3 tools/mock-peer/hop.py server --auto-confirm --exercise --duration 5
python3 tools/mock-peer/hop.py client --auto-confirm --duration 2
```

`--auto-confirm` exists only in the fake peer, never production pairing. `--exercise` sends synthetic pointer activity after authentication and ENTER ACK; connecting it to a real receiver can move/click its pointer, so do not accidentally target a working computer. Use explicit `--host <LAN_IP>` for LAN testing; `--direction 0` means HarmonyOS on the left,1 on the right. Mock `--binding` uses a0600 test fixture file, **not** the production token store; do not commit/share it.

### macOS

```sh
bash apps/macos/scripts/verify.sh
bash apps/macos/scripts/package.sh
# Requires complete Xcode:
bash apps/macos/scripts/xcode-build.sh
```

The CLT build compiles the actual native production sources for both architectures, targeting14.0. The real Xcode project is included, but a native CLT success is not an actual xcodebuild success. The development host currently lacks complete Xcode.

Output: `apps/macos/build/Hop.app` and universal application archive. The current package is **ad-hoc signed and not notarized**, not a Developer ID release. See [installation, permissions and signing](<docs/INSTALL_MAC.md>) and [implementation/build evidence](<apps/macos/IMPLEMENTATION.md>).

### HarmonyOS

Open the [DevEco project](<apps/harmonyos/>) using the HarmonyOS SDK including HMS desktop extensions, not an OpenHarmony-only SDK. Use minimumAPI20. The [SDK audit](<docs/SDK_AUDIT.md>) records the actual locally verified headers, ABI, library names, tooling and read-only device facts. In particular:

```c
Input_Result OH_Input_RequestInjection(Input_InjectAuthorizeCallback callback);
int32_t OH_Input_InjectMouseEventGlobal(const struct Input_MouseEvent* mouseEvent);
int32_t OH_Input_InjectKeyEvent(const struct Input_KeyEvent* keyEvent);
```

The asynchronous authorization callback is `void (*)(Input_InjectionStatus)`, without userdata. No invented signatures, CONTROL_DEVICE or accessibility bypasses are used.

Deployment requires developer mode and a valid developer-signed debug HAP/profile for **your device UDID**. An unsigned HAP is a build artifact, not an installable release. See [HarmonyOS build/sign/install instructions](<docs/INSTALL_HARMONYOS.md>). Never borrow another application's profile or publish private keys.

## First pairing

1. Use a trusted LAN; grant the Mac's Accessibility/Input Monitoring permissions and the PC's system input-injection consent.
2. Set left/right on the Mac and open its pairing window from the popover. Select the discovered Mac in the PC's statusbar panel.
3. Compare the same six-digit code and confirm on BOTH panels. Unilateral confirmation must not enable input.
4. Enable sharing on Mac, then cross the configured edge. The Mac must not hide/disassociate its cursor before the receiver's successful ENTER ACK.
5. Return at the opposite edge or use the emergency shortcut. Every failure must restore local input first and release remote pressed keys/buttons.
6. Existing bindings reconnect automatically; an explicit disconnect pauses reconnect. Unpair requires a new mutual confirmation; failed authentication never silently downgrades into pairing.

## Known limits

- Primary display only and one left/right neighbor, not arbitrary multi-display geometry. Physical key positions are sent; the target's layout/IME determines characters. Cmd maps to Meta, not Ctrl.
-120 SDK-defined Mac keycodes are documented;116 mapped/conditional entries. Fn, Keypad Clear and JIS Eisu/Kana deliberately remain unsupported rather than guessed. Media/brightness/vendor keys and OS-reserved shortcuts are not promised.
- The native SDK has no pressed-mouse-button-array setter; real single/multi-button drags need device testing. Input authorization and task/notification lifetime are system-controlled.
- Cross-compiling Intel does not verify Intel execution; compiling againstAPI26 or detecting anAPI26 device does not verify the HarmonyOS6/API20 minimum.
- No pre-login/secure-desktop input or promised auto-start. Actual debug-certificate/profile expiry depends on your account and issued credentials; do not assume fixed14/180-day validity.
- Physical handoff, fault recovery and SIGKILL behavior remain acceptance requirements. See the [plan](<PLAN.md>) for exact unresolved build/sign/runtime gates.
