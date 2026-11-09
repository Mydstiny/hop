# Hop — Mac → HarmonyOS PC 键鼠共享

[English](<README.en.md>) · [开发计划与真实验证记录](<PLAN.md>) · [线协议唯一规格](<docs/PROTOCOL.md>) · [双机验收清单](<docs/ACCEPTANCE.md>)

物理键鼠接在一台Mac，鼠标从约定的左/右边缘进入一台鸿蒙电脑，键盘跟随；在鸿蒙反向边缘推回Mac。**只做这一件事。**

## 当前状态

这是原生开发工程，不是已经通过双机验收的发布版。构建、离线测试、真实设备验证分开记录在[计划](<PLAN.md>)，不将mock成功宣称为真机成功。

- Mac：Swift + AppKit，菜单栏 `NSPopover`，`LSUIElement=true`，macOS14+，arm64/x86_64通用构建；主动 `CGEventTap`，恢复本地输入优先于网络操作。
- HarmonyOS：Stage ArkTS + NAPI C++，API20+；客户端仅注入，不尝试全局捕获；系统授权、顶栏面板、合法长时任务与常驻通知。
- 协议：自主Hop v1，单TCP连接、4字节大端长度前缀、128字节固定二进制体、10个类型；完整保留HELLO_ACK与KEY_DOWN/UP语义，详见[协议](<docs/PROTOCOL.md>)。
- 源码中的功能不等于真机已通过。尤其必须补验：真实授权、注入/拖拽、键位、左右穿越、后台常驻、急停/断网/SIGKILL恢复与最低版本兼容。

### 本次已验证结果

| 项目 | 结果 |
|---|---|
| Mac原生通用构建 | CLT脚本与**真实Xcode27.2 Release工程构建均通过**；产物复核arm64 + x86_64、最低14.0、LSUIElement=true |
| 鸿蒙DevEco构建 | **signed HAP官方SDK验签通过**，dev.hop.input/最低API20；2026-11-09 重装后顶栏注册成功、taskKeeping 任务57/通知38，`hideAbility` 后底部 Dock 不再出现 Hop；面板已改紧凑布局。桌面启动图标仍在。实际输入未验 |
| 离线 / localhost | 协议Python40测试、Swift644检查 + 8个TCP场景、C++354检查；真实Swift↔C++左右2个输入/返回场景通过（仅假注入SDK） |
| Xcode静态工程检查 | 40对象/53引用、全部10个Swift源文件及资源/scheme一致；17项正反测试通过，不代替xcodebuild |
| Mac原生面板离屏 | 284项真实AppKit布局/按钮检查、5个状态PNG；仅内存假数据、无可见窗口，不替代产品启动/菜单栏交互验收 |
| 只读运行检查 | 原生探针4项检查通过；18:28实际OS accessory角色、TCP24890监听与PC进程快照已复核，权限/配对/窗口/输入仍不能由此推断 |
| 真机输入 / 常驻 / 双机验收 | **未验证，不标M0–M4全部完成** |

最新完整离线回归12条命令exit0：合计100项Python测试（协议40、工程17、预检26、Xcode发现11、包元数据6），另有上表Swift/C++/TCP/AppKit检查。为保护正在运行的Mac，本次使用 `HOP_BUILD_DIR="$PWD/artifacts/test-build/live-safe-macos" python3 -B tools/verify.py --include-native-ui`，真实重新构建到隔离目录；原Mac PID与可执行文件SHA256前后不变，见[保留运行实例证据](<critique/mac-live-preservation.json>)。输出覆盖位置可变时，校验清单严格记录实际新包，不能误哈希旧默认包。

可复现总入口：`python3 tools/verify.py`。有WindowServer会话的Mac可显式选择 `python3 tools/verify.py --include-native-ui`，额外运行真实AppKit离屏内容视图测试；它创建测试用NSApplication但不显示窗口、不构造产品AppController或捕获/网络/存储服务，不读取真实权限。PNG与报告在构建目录，使用内存假数据，不是真机截图。详见[最终机器可读报告](<critique/build-test.json>)、[严格里程碑记录](<PLAN.md>)与[恢复真机验收的最短路径](<docs/RESUME.md>)。

只读验收预检：`python3 tools/device_preflight.py --target YOUR_DEVICE_TARGET --require-prerequisites`，详见[当前预检报告](<critique/device-preflight.json>)。工具安全性26项测试与11项Xcode发现测试通过不等于现场条件通过；Xcode27.2已完成首次设置且真实Release工程构建通过；签名HAP已通过官方验签，原PC调试连接已恢复并确认MOR-M1/2in1/API26。验签该产物时加 `--hap apps/harmonyos/entry/build/default/outputs/default/entry-default-signed.hap`；最新只读前置条件已通过，初次安装/启动见[历史部署记录](<critique/device-install.json>)，当前顶栏启动修复及锁屏拒绝见[最新修复部署](<critique/receiver-startup-fix.json>)；前置条件通过不表示设备已解锁，更不是权限/物理输入验收。默认exit0仅表示报告已收集，严格预检缺条件时exit2。它不安装、启动、弹权限或注入；多设备时不猜目标，不读取UDID/私钥。签名验证输出保存在本地忽略的私有产物目录。

在产品已运行时，可单独执行 `python3 -B tools/test_mac_runtime_probe.py --target YOUR_PC_TARGET`，隔离编译只读探针，不重建/重启产品，不触碰权限、配对或输入。探针exit0只表示该4项检查通过，不代表M1通过；见[运行证据](<critique/runtime-readiness.json>)和[本机首轮操作卡](<docs/M1_RUN.md>)。

## 安全说明（必读）

**v1默认仅用于可信局域网，token鉴权，明文传输。** 不使用TLS，也没有账号、云或公网穿透。配对码和token并不能防止局域网主动中间人，键鼠数据与token可被监听/篡改。不要在公共Wi-Fi、访客网或公网使用，不做公网端口映射，不在日志中输出token。

配对须两端核对同一个6位码并分别确认。token：Mac Keychain，鸿蒙Preferences。Mac急停：**Ctrl + Option + Command + Esc**。首次实测保留备用本地键鼠；崩溃后的系统光标清理仍须实际验证。

## 明确不做

剪贴板；跨机文件传输/文件拖拽；多屏显示扩展；手机/平板；鸿蒙当主机；账号/云/公网；应用市场发布；多台设备拓扑。只支持 **1 Mac + 1 HarmonyOS PC**、左/右二选一。远端同一应用/电脑内拖动窗口或选择文字属于鼠标输入，不是跨机文件拖拽。

不兼容Deskflow，不引入其协议、GPL服务端或源代码。参考资料中的旧Deskflow/TLS/剪贴板方案不属于本工程。

## 工程结构

```text
README.md / README.en.md
PLAN.md                       # 里程碑、失败与真实证据
 docs/PROTOCOL.md             # 唯一线协议
 docs/KEYMAP.md               # 从共享JSON生成的对照表
 docs/SDK_AUDIT.md            # 本机SDK真实签名与工具核验
 docs/ACCEPTANCE.md           # 待你逐项实测勾选
protocol/keymap.json          # 两端共同映射源
protocol/golden-vectors.json  # 共享黄金帧
apps/macos/                  # Swift + AppKit / Xcode
apps/harmonyos/              # ArkTS + NAPI / DevEco
tools/generate_keymap.py
tools/mock-peer/             # 标准库Python假对端与测试
```

全程产品实现使用系统原生API和标准库，没有第三方网络库或运行时产品依赖；DevEco自带的构建插件/编译器不属于产品依赖。

## 构建与测试

### 共享协议 / mock

要求Python3.10+，无需 `pip install`：

```sh
python3 tools/generate_keymap.py --check
python3 -m unittest discover -s tools/mock-peer -v
```

覆盖固定长度/大端黄金帧、分片粘包、非法输入、双确认/nonce/token/半配对、epoch、释放状态、心跳与移动合并，并包括真实localhost TCP与两个CLI假对端的左右布局回环。**不注入任何系统键鼠。**

### Mac

```sh
bash apps/macos/scripts/verify.sh
bash apps/macos/scripts/package.sh
# 需要完整Xcode时：
bash apps/macos/scripts/xcode-build.sh
```

[Mac安装/权限/签名教程](<docs/INSTALL_MAC.md>) · [Mac实现与验证边界](<apps/macos/IMPLEMENTATION.md>)。CLT构建和真实Xcode工程构建分开；本机完整Xcode27.2已完成真实Release构建。当前包为ad-hoc签名，未公证，不是Developer ID发行版。

### HarmonyOS PC

在DevEco Studio中打开[鸿蒙工程目录](<apps/harmonyos/>)，使用HarmonyOS SDK（包含HMS顶栏扩展，不是仅OpenHarmony SDK），最低API20。所需C API已在本机头文件核实：

```c
Input_Result OH_Input_RequestInjection(Input_InjectAuthorizeCallback callback);
int32_t OH_Input_InjectMouseEventGlobal(const struct Input_MouseEvent* mouseEvent);
int32_t OH_Input_InjectKeyEvent(const struct Input_KeyEvent* keyEvent);
```

授权回调是 `void (*)(Input_InjectionStatus)`，无userdata，不能靠记忆猜签名。实际头文件与链接库、工具命令见[SDK核验记录](<docs/SDK_AUDIT.md>)。

鸿蒙安装必须使用**开发者模式 + 自己设备的调试签名**；未签名HAP不等于可安装包。具体步骤见[鸿蒙安装教程](<docs/INSTALL_HARMONYOS.md>)。

## 无双机时使用假对端

两个终端，各运行一个命令；默认只绑定localhost：

```sh
# 假Mac，不捕获键鼠，默认需手动输入yes确认本假面板
python3 tools/mock-peer/hop.py server --host 127.0.0.1 --port 24890

# 假鸿蒙，不执行任何系统注入
python3 tools/mock-peer/hop.py client --host 127.0.0.1 --port 24890
```

纯自动离线演练（仅mock允许跳过它自己的面板确认；生产应用没有自动确认）：

```sh
python3 tools/mock-peer/hop.py server --auto-confirm --exercise --duration 5
python3 tools/mock-peer/hop.py client --auto-confirm --duration 2
```

`--exercise` 仅在认证和ENTER确认后发送合成鼠标序列；连接真实客户端时会导致实际鼠标注入，不可误连日常工作中的电脑。LAN测试需显式填写局域网IP，两端不能同时占用同一端口。`--direction 0` 为鸿蒙在左，1为右。mock的 `--binding` 是0600权限的**测试夹具文件**，不能替代生产Keychain/Preferences，不要提交或分享。

## 配对与恢复流程

1. 两台电脑在同一可信LAN，先授权Mac的辅助功能/输入监控、鸿蒙的系统注入许可。
2. Mac菜单栏选择方向，打开配对窗口；鸿蒙顶栏发现并选择Mac。
3. 核对同一6位码并两边确认。只确认一边不得有输入控制权。
4. Mac启用共享后，从指定边缘穿越；只有远端成功ENTER确认后才冻结Mac光标。
5. 返回边缘、禁用、断线、急停等均优先恢复本地输入；远端释放所有实际注入按键/按钮。
6. 已绑定设备自动重连；主动断开要保持断开。解绑后必须重新双确认，认证失败不能偷偷降级配对。

## 已知限制

- 主显示屏、左右单邻居，不支持任意多屏拓扑。输入法由目标系统处理，Cmd→Meta，不自动翻译成Ctrl。
- [映射表](<docs/KEYMAP.md>)涵盖120个SDK定义Mac键码；116有明确/条件映射，Fn、Keypad Clear、JIS英数/かな4项不猜测替换。媒体/亮度/厂商键及系统保留组合不承诺捕获。
- HarmonyOS SDK没有鼠标pressed-buttons数组setter；真实单/多按钮拖拽需设备验证。授权、保活、通知由系统决定，不旁路失败。
- 交叉编译不能替代Intel/macOS14运行；API26声明/设备不能替代HarmonyOS6/API20最低版本验收。
- 不支持登录前/安全桌面，不承诺开机自启动。调试证书/profile有效期依你的账号与实际签发结果，不能假定14天或180天。
- 当前构建、签名与未通过项目以[进度记录](<PLAN.md>)为准；完成[双机验收](<docs/ACCEPTANCE.md>)之前不要称为已可日用。

## English quick overview

Hop shares a Mac's physical keyboard/mouse with **one HarmonyOS PC**, switching at a left/right screen edge. Native AppKit menu bar on Mac; ArkTS/NAPI tray popup plus a legitimate continuous-task notification on HarmonyOS. No clipboard/files/cloud/mobile/reverse host/Deskflow compatibility. Hop v1 uses a single cleartext TCP stream and a fixed binary protocol. **Trusted LAN only; no TLS, confidentiality or active-MITM protection.** Confirm the same six-digit code on BOTH devices; token lives in Keychain/Preferences. Emergency: **Control+Option+Command+Escape**. Build/mock/localhost evidence is not a claim of hardware acceptance. See [English README](<README.en.md>), [Mac installation](<docs/INSTALL_MAC.md>), [HarmonyOS installation](<docs/INSTALL_HARMONYOS.md>) and the [unchecked real-device checklist](<docs/ACCEPTANCE.md>).
