# Local HarmonyOS SDK / DevEco audit

Audit date: 2026-11-08. Evidence is the installed SDK, declarations, official IDE templates, version/help commands, and read-only hdc queries. No other application source was copied, no package was installed, no device application was installed, and no input was injected during this audit. Presence of declarations is **not** proof of device behavior.

## 1. Installed versions and paths

| Component | Observed value | Evidence |
|---|---|---|
| DevEco Studio | 26.0.0, DS-261.23567.138.36.2600821 | [Info.plist:50–55](</Applications/DevEco-Studio.app/Contents/Info.plist#L50-L55>) |
| Default HarmonyOS SDK | API26, platform26.0.0, release26.0.0.105 | [sdk-pkg.json](</Applications/DevEco-Studio.app/Contents/sdk/default/sdk-pkg.json>), [Native package](</Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/oh-uni-package.json>), [ETS package](</Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/ets/oh-uni-package.json>) |
| Bundled Node | v24.14.1 | Executed `Contents/tools/node/bin/node --version`; [Hvigor README:45–47](</Applications/DevEco-Studio.app/Contents/tools/hvigor/hvigor/README.md#L45-L47>) requires >=24.14.1 |
| Hvigor / OHOS plugin | both6.26.4 | [Hvigor package](</Applications/DevEco-Studio.app/Contents/tools/hvigor/hvigor/package.json>), [OHOS plugin package](</Applications/DevEco-Studio.app/Contents/tools/hvigor/hvigor-ohos-plugin/package.json>), CLI `--version` |
| OHPM |26.0.0.630| [OHPM package](</Applications/DevEco-Studio.app/Contents/tools/ohpm/package.json>), CLI `--version` |
| Native tools | CMake3.28.2, Ninja1.13.2, OHOS clang15.0.4 | Executed bundled tool `--version` commands |
| hdc | Ver3.2.0f | Executed bundled `hdc -v` |
| Additional SDK | API23 /6.1.0.32 | [API23 Native package](</Users/mydestiny/Library/OpenHarmony/Sdk/23/native/oh-uni-package.json>) |

User SDK aliases resolve to another volume: `/Users/mydestiny/Library/OpenHarmony` → `/Volumes/MiniData/Developer/Toolchains/OpenHarmony`, `/Users/mydestiny/Library/Huawei/Sdk` → `/Volumes/MiniData/Developer/Toolchains/Huawei/Sdk`. API26 exists in the user OpenHarmony SDK too. Prefer the explicit bundled SDK for a reproducible audit. Runtime minimum must remain API20 even when compiling with API26.

## 2. Native injection API — actual ABI

Primary evidence: [oh_input_manager.h](</Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/include/multimodalinput/oh_input_manager.h>). It is Apache-2.0 SDK declaration material. Library annotation at line34 is **libohinput.so**. Native stubs are present for aarch64-linux-ohos, x86_64-linux-ohos, and arm-linux-ohos under the SDK sysroot. CMake may link `libohinput.so` (or linker `-lohinput`). NAPI additionally links `libace_napi.z.so`, as demonstrated by the [official CMake template](</Applications/DevEco-Studio.app/Contents/plugins/openharmony/lib/templates/ability/Native C++/code template/cpp/CMakeLists.txt.ftl>).

### Authorization

Exact declarations, omitting availability attributes:

```c
typedef enum Input_InjectionStatus {
    UNAUTHORIZED = 0,
    AUTHORIZING = 1,
    AUTHORIZED = 2
} Input_InjectionStatus; // since20

typedef void (*Input_InjectAuthorizeCallback)(Input_InjectionStatus authorizedStatus);
Input_Result OH_Input_RequestInjection(Input_InjectAuthorizeCallback callback); // since20
Input_Result OH_Input_QueryAuthorizedStatus(Input_InjectionStatus* status);      // since20
void OH_Input_CancelInjection();                                               // since12
```

Evidence: [status enum:269–289](</Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/include/multimodalinput/oh_input_manager.h#L269-L289>), [callback:599–604](</Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/include/multimodalinput/oh_input_manager.h#L599-L604>), [authorization functions:1758–1805](</Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/include/multimodalinput/oh_input_manager.h#L1758-L1805>).

- Callback has **one enum argument, no userdata**, and is asynchronous. Query returns an operation result and writes the enum through a pointer; it does not return a boolean authorization flag.
- `RequestInjection` result0 means wait for user authorization callback. Results include401 invalid callback,801 unsupported,3800001 service error,3900005 authorizing,3900006 repeated request <=3seconds,3900007 already authorized,3900008 another application authorized.
- `Input_Result` is a separate enum, not `Input_InjectionStatus`; do not confuse result3900007 with status2.
- Since API20 the headers advise request+query before injecting. API26 documents direct injection for holders of CONTROL_DEVICE, but Hop must **not** request that bypass permission. Use the user authorization path only.
- Header does not promise callback thread or process lifetime; marshal notifications with NAPI threadsafe functions, serialize input, and recheck authorization before processing. Revocation must release the actual pressed set and disconnect.

### Key events

[Key API:733–901](</Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/include/multimodalinput/oh_input_manager.h#L733-L901>):

```c
struct Input_KeyEvent* OH_Input_CreateKeyEvent();
void OH_Input_DestroyKeyEvent(struct Input_KeyEvent** keyEvent);
void OH_Input_SetKeyEventAction(struct Input_KeyEvent*, int32_t action);
void OH_Input_SetKeyEventKeyCode(struct Input_KeyEvent*, int32_t keyCode);
void OH_Input_SetKeyEventActionTime(struct Input_KeyEvent*, int64_t microsecondsSinceBoot);
void OH_Input_SetKeyEventWindowId(struct Input_KeyEvent*, int32_t); // since15
void OH_Input_SetKeyEventDisplayId(struct Input_KeyEvent*, int32_t); // since15
int32_t OH_Input_InjectKeyEvent(const struct Input_KeyEvent*);
```

Create returns nullable pointer; destroy takes **pointer to pointer**. Action enum: CANCEL0, DOWN1, UP2. Key codes are HarmonyOS keycodes, not macOS virtual codes, USB HID usages, or Unicode. Use the shared mapping source; never guess.

**No key-event modifier-mask setter, pressed-keys array setter, or repeat setter exists in this header.** Modifier keys are individual injected keydown/keyup events. API22 documentation explicitly warns that injected Meta/Ctrl/Alt/Shift/CapsLock/ScrollLock/NumLock downs must have timely ups to prevent stuck keys. Repeating a nonmodifier key requires ordered down injection; actual OS repeat behavior needs device validation. CapsLock state must be adapted to lock semantics, not held indefinitely.

`OH_Input_SetKeyPressed(Input_KeyState*, int32_t)` and `OH_Input_SetKeySwitch(Input_KeyState*, int32_t)` operate on query-state objects, **not** an injection modifier mask. The modifier preKeys methods around line2401 apply to `Input_Hotkey` registration, **not** key-event injection.

### Mouse / scroll / held-button limitations

[Mouse API:977–1334](</Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/include/multimodalinput/oh_input_manager.h#L977-L1334>):

```c
struct Input_MouseEvent* OH_Input_CreateMouseEvent();
void OH_Input_DestroyMouseEvent(struct Input_MouseEvent**);
void OH_Input_SetMouseEventAction(struct Input_MouseEvent*, int32_t);
void OH_Input_SetMouseEventDisplayX(struct Input_MouseEvent*, int32_t pixels);
void OH_Input_SetMouseEventDisplayY(struct Input_MouseEvent*, int32_t pixels);
void OH_Input_SetMouseEventButton(struct Input_MouseEvent*, int32_t);
void OH_Input_SetMouseEventAxisType(struct Input_MouseEvent*, int32_t);
void OH_Input_SetMouseEventAxisValue(struct Input_MouseEvent*, float);
void OH_Input_SetMouseEventActionTime(struct Input_MouseEvent*, int64_t microsecondsSinceBoot);
void OH_Input_SetMouseEventWindowId(struct Input_MouseEvent*, int32_t); // since15
void OH_Input_SetMouseEventDisplayId(struct Input_MouseEvent*, int32_t); // since15
void OH_Input_SetMouseEventGlobalX(struct Input_MouseEvent*, int32_t pixels); // since20
void OH_Input_SetMouseEventGlobalY(struct Input_MouseEvent*, int32_t pixels); // since20
int32_t OH_Input_InjectMouseEvent(const struct Input_MouseEvent*);
int32_t OH_Input_InjectMouseEventGlobal(const struct Input_MouseEvent*); // since20
```

- Actions: CANCEL0, MOVE1, BUTTON_DOWN2, BUTTON_UP3, AXIS_BEGIN4, AXIS_UPDATE5, AXIS_END6.
- Buttons: NONE=-1, LEFT0, MIDDLE1, RIGHT2, FORWARD3, BACK4. Protocol button ordering differs: translate explicitly.
- Axis: VERTICAL0, HORIZONTAL1. `axisValue` is float; positive is documented as forward, negative backward,1.0 one logical unit. Protocol signed1/120-step convention needs a conscious conversion and real-device direction test.
- Global coordinates are pixels with primary screen top-left origin; display coordinates have the selected screen top-left origin. Do not use relative deltas as global positions.
- **No held/pressed mouse-buttons array/bitmask setter exists**, in either audited API26 or API23 header. Maintain ordered down/up state locally and do not invent an SDK function. Drag with one/multiple held buttons must be device-tested; declaration presence alone does not establish how injection service preserves held state during MOVE.
- All created objects must be destroyed, including failures. Release known injected downs on leave/disconnect; perform releases before revoking your own injection grant where possible.

## 3. PC statusbar/tray and native panel

Use exact import spelling **`@kit.DeskTopExtensionKit`**, capital T in `Top`. [Kit declaration](</Applications/DevEco-Studio.app/Contents/sdk/default/hms/ets/kits/@kit.DeskTopExtensionKit.d.ts>) is since6.0.0(API20), exports `statusBarManager` and `StatusBarViewExtensionAbility`. The older [StatusBarExtensionKit](</Applications/DevEco-Studio.app/Contents/sdk/default/hms/ets/kits/@kit.StatusBarExtensionKit.d.ts>) is deprecated since API20. These are **HMS declarations**, not plain OpenHarmony-only APIs: build with HarmonyOS SDK and `runtimeOS: HarmonyOS`.

Evidence: [statusBarManager declaration](</Applications/DevEco-Studio.app/Contents/sdk/default/hms/ets/api/@hms.pcService.statusBarManager.d.ts>), [extension class](</Applications/DevEco-Studio.app/Contents/sdk/default/hms/ets/api/@hms.pcService.StatusBarViewExtensionAbility.d.ts>).

```ts
class StatusBarViewExtensionAbility extends UIExtensionAbility {}
function addToStatusBar(context: common.Context, item: StatusBarItem): void;
function removeFromStatusBar(context: common.Context): void;
function updateStatusBarMenu(context: common.Context, groups: StatusBarGroupMenu[]): void;
function updateStatusBarIcon(context: common.Context, icon: StatusBarIcon): void;
function updateQuickOperationHeight(context: common.Context, height: number): void;
function on(type: 'rightMenuClick', cb: Callback<emitter.EventData>): void;
```

- Syscap `SystemCapability.PCService.StatusBarManager`, Stage only. Only one icon per app. Accepted contexts: UIAbilityContext, ServiceExtensionContext, FormExtensionContext. Do not pass UIExtensionContext blindly.
- Required `StatusBarItem.icons`: `white` and `black` PixelMaps, recommended24vp square. Required `quickOperation`: `{ title: string, height: number (>0), abilityName: string, moduleName?: string }`. This points to the custom StatusBarViewExtensionAbility for a native ArkUI popup. API20 optional `loadingStatus` exists.
- Right-click `statusBarGroupMenu?: StatusBarGroupMenu[]`; each group is an array of menu items. <=20 first-level entries and <=20 children per entry.
- For API20 use `StatusBarMenuAction { abilityName, moduleName?, notifyOnly?: boolean, menuCode?: string }`. `notifyOnly=true` plus `menuCode` enables `rightMenuClick` event delivery. Top-level item `menuCode` / options are **API24**, not API20.
- `hoverTips` / update hover tips are **API22**. `isStatusBarCapabilitySupported` and hover callbacks are **API26**. Do not call them for an API20 minimum; use `canIUse` for the syscap and handle API errors.
- Removing statusbar icon can throw1010710004 when no window is foreground. Restore/exit workflow must not assume removal always succeeds headlessly.
- Tray presence alone does not establish process residency; pair it with the legitimate PC long-running mode below. A UIAbility may be started without leaving its main window visible; validate actual system launch/popup behavior on device.

## 4. Long-running background modes

Evidence: [backgroundTaskManager:665–771](</Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/ets/api/@ohos.resourceschedule.backgroundTaskManager.d.ts#L665-L771>), [modes:1110–1383](</Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/ets/api/@ohos.resourceschedule.backgroundTaskManager.d.ts#L1110-L1383>).

```ts
function startBackgroundRunning(context: Context, mode: BackgroundMode, wantAgent: WantAgent): Promise<void>;
function stopBackgroundRunning(context: Context): Promise<void>;
// BackgroundMode.TASK_KEEPING = 9
```

Requires `ohos.permission.KEEP_BACKGROUND_RUNNING`, declared matching ability background mode, UIAbility context in Stage, and WantAgent notification target. Successful continuous task creates a notification without prompt sound. Use legitimate **TASK_KEEPING9** on PC/2-in-1: declarations expressly say API20 and earlier PC/2-in-1 only; API21+ non-PC requires ACL `KEEP_BACKGROUND_RUNNING_SYSTEM` but PC does not have that non-PC restriction. This is not a promise that every arbitrary task will be accepted: handle9800005 verification failure and notification failures.

Do **not** disguise idle input sharing as audio playback or data transfer. DATA_TRANSFER1 is upload/download and is canceled if progress is not updated for >10minutes. MULTI_DEVICE_CONNECTION6 is documented for distributed service connection/casting, not generic TCP residency. New `BackgroundTaskMode` / `ContinuousTaskRequest` overloads are API21+, special-scenario processing mode13 is API22 and ACL-gated (API24 special-scenario ACL); none is needed for API20 PC taskKeeping. A statusbar UIExtensionAbility itself cannot request the Stage UIAbility-only continuous task.

### Actual cold-start failure found on the PC (2026-11-08)

Own-PID logs reported `The size of the pixelmap exceeds the limit` and notification `Ans: null uiContent`, followed by Hop startup failure. The [icon contract](</Applications/DevEco-Studio.app/Contents/sdk/default/hms/ets/api/@hms.pcService.statusBarManager.d.ts#L81-L104>) recommends24 **vp**, not raw pixels. The receiver now derives pixel dimensions from display density and awaits the callback overload of addToStatusBar (it is not a Promise-returning API). Callback success is registration evidence only, not visual acceptance.

The [notification precondition](</Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/ets/api/@ohos.notificationManager.d.ts#L770-L799>) explicitly requires loaded UI content before requestEnableNotification. Calling it from a deliberately contentless main UIAbility was invalid. Do not create a visible main-window workaround or cast/spoof another context. The receiver instead requests legitimate TASK_KEEPING and lets the platform create its required notification; any9800006 notification verification failure still blocks connection. API20 [getAllContinuousTasks](</Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/ets/api/@ohos.resourceschedule.backgroundTaskManager.d.ts#L971-L990>) is queried through the owner context, filtered to Hop/EntryAbility, and only its own active task/notification IDs are logged. This is not a global task dump.

The fixed signed HAP built/verified/installed, but normal launch was refused with10106102 because the PC is locked. Thus actual registration, task and notification success of this fix remain **unverified**. Unlock normally as the operator; do not disable developer mode or bypass the lock just because CLI diagnostic text suggests a workaround. See [deployment evidence](<../critique/receiver-startup-fix.json>).

## 5. Build availability, caches, commands

Bundled official templates are available; no external project source is needed:

- [New Project template](</Applications/DevEco-Studio.app/Contents/plugins/openharmony/lib/templates/project/New Project/template.json>), [root build profile template](</Applications/DevEco-Studio.app/Contents/plugins/openharmony/lib/templates/project/New Project/code template/build-profile.json5.ftl>), [root hvigorfile](</Applications/DevEco-Studio.app/Contents/plugins/openharmony/lib/templates/project/New Project/code template/hvigorfile.ts>).
- [Native C++ template](</Applications/DevEco-Studio.app/Contents/plugins/openharmony/lib/templates/ability/Native C++/template.json>), [native module profile](</Applications/DevEco-Studio.app/Contents/plugins/openharmony/lib/templates/ability/Native C++/code template/module-build-profile.json5.ftl>), [local NAPI dependency declaration](</Applications/DevEco-Studio.app/Contents/plugins/openharmony/lib/templates/ability/Native C++/code template/module-oh-package.json.ftl>), [native type package](</Applications/DevEco-Studio.app/Contents/plugins/openharmony/lib/templates/ability/Native C++/code template/cpp/types/libhello/oh-package.json5.ftl>).
- [Module hvigorfile](</Applications/DevEco-Studio.app/Contents/plugins/openharmony/lib/templates/module/New Module/code template/hvigorfile.ts>) uses `hapTasks`; root uses `appTasks`. NAPI types are a **local file dependency**, not a registry dependency.

SDK includes ArkTS compiler/resource tools, packaging/signing tools, native sysroot and compiler; Hvigor plugin ships183MiB of its own dependencies. Existing user Hvigor wrapper tools cache is39MiB (pnpm infrastructure), OHPM cache192KiB with four gzip package archives (test/mock package layouts and one compiled HAR layout). The cache is **not** evidence that arbitrary registry packages are available. A minimal Stage+NAPI app can avoid external product dependencies and omit generated hypium/hamock test dependencies. Package install commands were deliberately not run during audit.

Concrete paths and candidate build command (run from actual project root after writing the project):

```sh
export DEVECO=/Applications/DevEco-Studio.app/Contents
export NODE_HOME="$DEVECO/tools/node"
export PATH="$NODE_HOME/bin:$DEVECO/tools/ohpm/bin:$PATH"
export DEVECO_SDK_HOME="$DEVECO/sdk"
export JAVA_HOME="$DEVECO/jbr/Contents/Home"
"$NODE_HOME/bin/node" "$DEVECO/tools/hvigor/bin/hvigorw.js" --mode module \
  -p product=default -p module=entry@default -p buildMode=debug assembleHap \
  --no-daemon --no-parallel --stacktrace
```

Both wrapper and direct Hvigor `--help` and version calls succeeded. `--no-daemon`, `--no-parallel`, and `--stacktrace` are confirmed options; **`--offline` is not listed**, so do not invent it. Plugin resolution, modelVersion compatibility, SDK selection, local NAPI OHPM resolution, compile and packaging must be verified with the first real empty build. Empty build had not yet been attempted at audit completion. The New Project bundled hvigor-wrapper.js template is zero bytes: use installed real wrapper, not that placeholder.

Native tools:

```text
/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/build-tools/cmake/bin/cmake
/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/build-tools/cmake/bin/ninja
/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/llvm/bin/clang
/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/build/cmake/ohos.toolchain.cmake
```

Unsigned HAP creation and deployable signed HAP are separate. Signing needs appropriate app certificate/profile/device authorization; do not copy signing material from another project or silently claim an unsigned package installs. Do not modify global SDK/tool/cache locations without approval. Prefer bundled Node explicitly: OHPM shell launcher tests PATH node before NODE_HOME.

## 6. hdc read-only evidence

Executed bundled `hdc -v`, `hdc list targets`, then target-specific `shell param get` only. All succeeded with exit0.

```text
Target: 192.168.31.140:44491
const.product.model: MOR-M1
const.product.devicetype: 2in1
const.ohos.fullname: OpenHarmony-7.0.0.109
const.ohos.apiversion: 26
```

Device is classified **2in1**, not a phone. No install, app start, permission dialog, or injection was attempted. Parent coordination is required before any device installation/injection. Device availability now is not proof of API20 behavior or application signing readiness.

## 7. Audit command failures / constraints

- An exploratory directory glob for `Contents/plugins/deveco*` had no matches; actual official template plugin is `Contents/plugins/openharmony`, with HMS additions under `plugins/harmony`.
- First guessed Ninja path `native/build-tools/ninja/ninja` failed exit1; corrected through glob discovery to `native/build-tools/cmake/bin/ninja`, version call succeeded exit0.
- QuickBar companion declaration is `.d.ets`, not `.d.ts`; irrelevant to required statusbar integration. The required statusBarManager and extension `.d.ts` files exist and were read.
- No installations, system modifications, source borrowing, GPL source, device install, or injection were needed for the audit. No claim of build/runtime success is made before corresponding actual evidence.
