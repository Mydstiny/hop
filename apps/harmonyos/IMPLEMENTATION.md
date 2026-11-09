# HarmonyOS PC receiver implementation / verification

## Target and boundaries

- Native HarmonyOS PC / 2-in-1 Stage app, minimum HarmonyOS 6 / API20; compiled with the installed HarmonyOS API26 SDK. Product versions are `targetSdkVersion: "26.0.0"` and `compatibleSdkVersion: "6.0.0(20)"`. The mixed syntax is required by the installed Hvigor plugin: API26+ uses the new dotted API format, earlier HarmonyOS versions use platform/API notation.
- Receive-only ArkTS + NAPI/C++; one Mac, one TCP session, no third-party networking library. The wire format remains the canonical [protocol specification](../../docs/PROTOCOL.md).
- No global input subscriptions, accessibility service, input interception, `CONTROL_DEVICE`, clipboard, file transfer, TLS, cloud, or account integration. The only input state query other than authorization is the SDK's CapsLock lock-state query, not an event listener.
- Plaintext TCP plus bearer token is for a trusted LAN only. The matching six-digit code is a user confirmation aid, not protection against an active MITM.

## Implemented pieces

| Piece | Implementation |
|---|---|
| Binary codec | [protocol.h](entry/src/main/cpp/protocol.h): fixed 128-byte bodies / 132-byte frames, endian-safe serialization, bounded fragmented/coalesced decoder, field/reserved-byte/UTF-8/phase validation. READY and REJECT may retain a valid pairing code; HELLO flags and phase must match role. |
| Pair state | [pair_state.h](entry/src/main/cpp/pair_state.h): fresh nonce, matching peer/code/token, local confirmation barrier, durable-storage barrier before COMMIT acknowledgment, idempotent duplicate COMMIT/storage completion, bound RESUME and no automatic downgrade after REJECT. |
| TCP receiver | [client.cpp](entry/src/main/cpp/client.cpp): ordered native worker, numeric IPv4/IPv6 endpoints, nonblocking connection/read/write, short-write handling, 1-second heartbeats, 3-second receive deadline, 60-second pairing deadline, increasing nonzero epochs, ENTER success acknowledgment, return-edge arming and 300ms leave deadline. |
| User-authorized injection | [injector.h](entry/src/main/cpp/injector.h): actual SDK request/query authorization, primary-display global mouse coordinates, explicit protocol-to-SDK buttons, generated physical-key map, modifier reconciliation, CapsLock lock semantics, repeat, wheel begin/update/end, actual pressed-set cleanup. Downs are tracked before injection; failed wheel updates still end the axis and failed axis-end is retried during cleanup. |
| NAPI | [napi_bridge.cpp](entry/src/main/cpp/napi_bridge.cpp): per-environment threadsafe-function callbacks, no token in public state snapshot, bounded callback queue, serialized client control, environment cleanup. Asynchronous Preferences success/failure acknowledgments include the exact session nonce, so an older flush cannot approve a newer handshake. |
| Preferences / discovery / reconnect | [HopService.ets](entry/src/main/ets/HopService.ets): secure-random persistent UUID, binding/token/address in Preferences, `flush()` before COMMIT acknowledgment, `_hop._tcp` discovery with version/UUID TXT validation, manual IP fallback, bounded exponential reconnect, explicit disconnect pause, authenticated-unpair cleanup after native input release. Initialization, native calls, timers, and storage failures are handled without an input-capture fallback. |
| Statusbar-only interface | [EntryAbility.ets](entry/src/main/ets/entryability/EntryAbility.ets), [HopStatusBarAbility.ets](entry/src/main/ets/entryability/HopStatusBarAbility.ets), [Index.ets](entry/src/main/ets/pages/Index.ets): actual `@kit.DeskTopExtensionKit`, light/dark PixelMap tray icon, system-owned popup, pair code/confirm, permission action, discovered/manual connections, disconnect/unpair, right-click disconnect/quit. |
| No main-window UI | Entry never calls main-window `loadContent`, `setUIContent`, or `showWindow`. It removes the starting window, sets `excludeFromDock`, and after status-bar registration calls `hideAbility()`. `moveAbilityToBackground()` is unsupported on this 2-in-1 (16000061). Only the statusbar extension loads the ArkUI page. |
| Legitimate residency | `TASK_KEEPING` PC continuous task with `KEEP_BACKGROUND_RUNNING`, WantAgent, and notification authorization. The platform supplies the required persistent notification. No fake audio/data-transfer mode or ACL permission. Connection attempts are disabled if the background task/notification cannot start. |

[CMakeLists.txt](entry/src/main/cpp/CMakeLists.txt) now compiles the real bridge and client and links the audited SDK libraries. The unused original NAPI/version skeleton and one-shot skeleton generator were removed. The [generated keymap](entry/src/main/cpp/generated/keymap.h) was not hand-edited; [the shared mapping](../../protocol/keymap.json) remains its only source.

## Reproduce verification

Run from `apps/harmonyos`:

```sh
# Host-side actual portable C++, fake SDK only. No physical injection.
python3 tests/run_native_tests.py

# Actual DevEco compile, resources, ArkTS, native linkage, unsigned packaging.
bash scripts/build.sh

# Both; writes structured status and raw logs.
bash scripts/verify.sh
```

The [build wrapper](scripts/build.sh) preserves the originally approved command, bundled Node/Java/SDK paths, and existing tool-cache configuration. It does not install dependencies, relocate denied cache writes, modify global configuration, copy signing material, or use a device. A restricted child must ask its parent to execute this command if Hvigor's pre-existing external cache is denied by that child's sandbox.

Reports are generated under `tests/.build/`:

- `native-tests-report.json`: actual exit status, check count, golden-frame count, tested source list, and explicit fake-SDK/no-device boundary.
- `hap-build-report.json`: actual tool exit status, minimum/compile API, unsigned HAP path/size/SHA-256, and timestamp. This is the authoritative current artifact checksum; do not reuse a checksum from an earlier build.
- `build-test.json`: `build.passing` and `tests.passing` convergence signals plus exit codes and logs.
- `native-tests.log` and `hap-build.log`: complete command output from the combined wrapper.

### Verified offline coverage

The native suite compiled the **actual** codec, PairState, Injector, and Client using host clang++, a test-only recording SDK surface, and loopback TCP. The frozen native implementation passed **354 checks against the same 12 canonical golden frames** with exit0. An additional build of the same suite with `-fsanitize=address,undefined -fno-omit-frame-pointer` also passed all 354 checks with exit0 and no sanitizer diagnostics. It does not link the system input service.

Coverage includes golden bytes/roundtrips, bytewise fragmentation/coalescing, malformed length/version/reserved data, HELLO role/flags, nonce/code/direction/identity/token mismatch, no token before local confirmation, no READY before durable storage completion, duplicate COMMIT/storage completion, no RESUME-to-PAIR downgrade, mouse/key/modifier/CapsLock/button mapping, repeat, wheel phases and failed-axis cleanup, actual pressed-set release, fake SDK allocation balance, actual Client pairing/RESUME, no pre-authentication injection, stale epochs, return-edge release/300ms timeout, 3-second heartbeat timeout, injection failure BYE3, and authorization-revocation release attempts.

## Build artifact and limitations

The actual integrated DevEco native/ArkTS/resource/packaging build has produced:

- [entry-default-unsigned.hap](entry/build/default/outputs/default/entry-default-unsigned.hap), arm64-v8a.
- **Unsigned**: `signingConfigs` is empty. A Hvigor stage named SignHap does not make this signed. No installability or device authorization is claimed. The generated report records the latest exact size/hash.

Remaining compiler diagnostics include the installed toolchain's upstream `--gcc-toolchain` warning and its generic native-module / ArkTS potentially-throwing-call notices. The source uses explicit error boundaries; warning suppression was not used to manufacture a pass.

Not verified by a successful build or fake-SDK test:

- Signed installation/startup and the real statusbar popup/process-sharing lifecycle.
- No-mainwindow appearance during launch, notification clicks, and system authorization UI.
- Actual TASK_KEEPING acceptance, persistent notification interaction, background retention, sleep/wake, and network transitions.
- Real injection authorization grant/revoke behavior and OS cleanup on process death.
- Actual mouse movement, held-button drag semantics, wheel direction, keyboard layouts/system shortcuts/repeat, CapsLock restoration, edge crossing, and full two-machine safety behavior.
- Runtime compatibility on an API20 machine. Read-only discovery of a MOR-M1 2-in-1 running API26 is not API20 runtime proof.

No application installation, device input, signing credential copying, global SDK/cache reconfiguration, or accessibility/ACL bypass was performed for this implementation. See the [SDK audit](../../docs/SDK_AUDIT.md) and [plan](../../PLAN.md) for the broader milestone/device acceptance boundaries.
