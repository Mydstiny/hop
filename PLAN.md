# Hop v1 开发计划与验证记录

## 目标与优先级

物理键鼠接在 Mac；Mac 服务端 → 一台 HarmonyOS PC 客户端。左/右二选一的边缘穿越，键盘跟随。仅键鼠共享、边缘穿越、可信局域网配对。两端无主窗口，顶栏小面板操作。

本次用户提示词优先于参考 HTML（`/Users/mydestiny/Downloads/鸿蒙与 Mac 键鼠共享调研与设计计划.html`）。参考中的 Deskflow 协议/GPL 服务端、TLS、剪贴板、fork 方案全部不采用；仅允许查阅注入调用方式，不拷贝其代码。

## 硬性边界

- 不做剪贴板、文件传输/跨机拖拽、多屏显示扩展、手机/平板、鸿蒙主机、云/账号/公网、多机拓扑、应用市场发布。
- Mac：Swift + AppKit，macOS 14+，Apple Silicon/Intel，LSUIElement=true，主动 CGEventTap，辅助功能引导，Keychain 存 token。
- Mac 任何离开/异常/断连/急停（Ctrl+Opt+Cmd+Esc）路径，首先恢复本地输入和光标，再处理网络与 UI。不得让用户失去本地键鼠。
- HarmonyOS：HarmonyOS 6 / API20+，Stage ArkTS + NAPI C++。只注入，禁止全局捕获；用户弹窗授权，不使用无障碍/ACL 旁路。Preferences 存 token；顶部状态栏/托盘与长时任务常驻通知。
- 单 TCP 长连接，4 字节大端长度前缀，定长二进制消息体，无 JSON 线协议、无第三方网络库；移动合并只保留最新值；1 秒心跳，3 秒超时。
- mDNS `_hop._tcp`；双端显示相同六位码并均确认后交换随机 token，支持解绑、自动重连。明文 + token，仅可信局域网；不添加 TLS。
- API 红线：必须先在本机 SDK 找到真实注入 API 签名；不存在即停工报告。构建错误先修，同一问题失败三次才报告；不伪造构建或真机结果。每个真正完成的里程碑独立 git 提交。

## 目录与职责

```
README.md
PLAN.md
docs/PROTOCOL.md   # 唯一线协议规格，先于实现
 docs/KEYMAP.md
protocol/keymap.json # 两端共用源映射，生成平台资源而非手写两份
apps/macos/        # Xcode 工程 + 可用 CLT 验证的原生构建入口
apps/harmonyos/    # DevEco 工程，ArkTS + NAPI
tools/mock-peer/   # Python 标准库假对端与协议测试
```

## 协议歧义的明确决策

用户称“仅10种”但枚举包含12个消息名。定义10个顶层二进制类型：HELLO/HELLO_ACK 使用相同类型的 ACK 位；KEY_DOWN/KEY_UP 使用同一 KEY 类型的 action 字段。所有列出的语义保留，不增加新的逻辑消息。完整布局、长度、校验、握手和状态转换只以 `docs/PROTOCOL.md` 为准。

## 里程碑 / 出口标准

- [ ] **M0**：两工程空构建通过；PROTOCOL/keymap 定稿；mock-peer 帧编解码自测通过。
- [ ] **M1**：手填 IP 连通；鸿蒙真机授权后注入鼠标移动成功。
- [ ] **M2**：左右布局双向边缘穿越，点击、拖拽、滚轮；离开释放注入状态。
- [ ] **M3**：物理键位映射、左右修饰键、常用组合、重复/释放、异常清理。
- [ ] **M4**：mDNS、双确认配对、持久化、重连、解绑、顶栏/常驻通知；断线与急停恢复全测。
- [ ] **M5**：双机实测待勾清单、中英 README、安装包与签名/安装教程；明确构建与真机验证边界。

每步运行能运行的构建/测试并记录命令、结果与环境。真机条件不具备时，相关出口保持未通过，不能把 mock 通过冒充真机通过。不因缺少真机而停止可以完成的源码、离线测试与文档工作。

## 前置 SDK 核验（2026-11-08）

已使用工具 grep + read 核实：

头文件：`/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/include/multimodalinput/oh_input_manager.h`

```c
// L759，since 12
int32_t OH_Input_InjectKeyEvent(const struct Input_KeyEvent* keyEvent);
// L1022，since 20
int32_t OH_Input_InjectMouseEventGlobal(const struct Input_MouseEvent* mouseEvent);
// L1780，since 20
Input_Result OH_Input_RequestInjection(Input_InjectAuthorizeCallback callback);
```

结论：API 红线检查通过。授权为异步回调，不是无参数或同步 bool；用户提示词未给出具体签名，因此不存在签名冲突。后续只按 SDK 声明实现，先授权再注入，不申请 CONTROL_DEVICE。

## 进度 / 事实记录

### 启动检查

- 2026-11-08：`pwd` 确认根目录 `/Users/mydestiny/Developer/mouse_transfer`，最初为空，无现存代码；已先建立所要求目录。
- 已读取参考 HTML 的设计计划与来源部分；其范围冲突已按上文明确排除。
- SDK 三项 API 均存在，未触及停工红线。
- `xcode-select -p` 为 `/Library/Developer/CommandLineTools`。`xcodebuild -version` 实际失败：需要完整 Xcode；当前常见路径没有 Xcode.app。Swift 6.4 可用。不会把 xcodebuild 文件存在误报为已安装完整 Xcode；继续检查可用工具并使用 CLT 验证可验证部分，不篡改全局 xcode-select。
- DevEco Studio 与 bundled SDK/build tools 存在；已完成SDK真实API签名、链接库、ArkTS 顶栏/后台声明和构建入口核验，详细证据见 [SDK_AUDIT](docs/SDK_AUDIT.md)。

### 实现与验证检查点（2026-11-08）

严格出口仍以开头勾选项为准。环境缺少完整Xcode与本工程的有效设备调试签名，因此没有把M0–M4标成完成；并行完成源码、离线验证与文档，不意味着跳过真机出口后仍宣称里程碑通过。

| 里程碑 | 已完成的源码 / 自动验证 | 严格出口仍欠缺 |
|---|---|---|
| M0 | 协议先落盘、120个SDK定义Mac键码（116映射/条件映射、4明确不支持）、12黄金帧；Python40测试通过；Mac空工程先经CLT通用构建通过；完整原生Mac与DevEco HAP构建通过 | 没有完整Xcode，真实xcodebuild未运行；鸿蒙空工程阶段遇环境/版本配置错误，修好时已接入完整实现，不能倒称空工程先通过 |
| M1 | 手填数字IPv4/IPv6、原生TCP握手、授权回调/查询/注入路径；真实C++ Client链接假SDK的移动/会话测试 | 本工程尚未调试签名、安装和真实系统授权/鼠标移动验证 |
| M2 | ENTER精确ACK后才隐藏/冻结；虚拟坐标、120Hz单槽、按钮/滚轮、左右边缘返回、超时释放；对应离线测试 | 真实双机左右穿越、鼠标速度/DPI、单/多按钮拖拽与滚轮方向 |
| M3 | 共用映射、左右修饰键、CapsLock、重复/释放、异常集合清理；Mac644检查、鸿蒙354检查（含假注入）通过 | 真机逐键、组合键、输入法、系统保留键与硬件差异 |
| M4 | 双确认/token落盘屏障、Keychain/Preferences实现、mDNS、重连/解绑、顶栏与TASK_KEEPING源码；Mac8个真实localhost TCP场景、真实Swift↔真实C++ localhost配对互通通过 | 真正Keychain/Preferences/Bonjour/权限/托盘通知与后台验证；真实断线、急停、SIGKILL光标清理 |
| M5 | 中英README、两端安装/签名教程、逐项未勾的双机验收表；Mac ad-hoc通用包、鸿蒙unsigned HAP | 用户自己的有效鸿蒙调试签名/profile/UDID；真实可安装签名包未生成，不冒充已生成 |

### 构建问题与修复事实

1. Hvigor首次被沙箱拒绝写入用户既有全局缓存（不是SDK API缺失）。父会话按原命令申请授权重试，没有改全局设置或换路径绕过拒绝。随后用户将父会话策略改为danger-full-access，后续父会话正常构建。委派子会话仍保持各自固定沙箱，必要HAP构建由父会话执行。
2. `targetSdkVersion: "26.0.0(26)"` 导致SDK参数格式错误。尝试数字26/20后被HarmonyOS schema要求字符串拒绝；查真实plugin错误说明后修为 **target `"26.0.0"`、compatible `"6.0.0(20)"`**，未提高最低API。两类配置错误均已修复，无同一问题盲试三次。
3. ArkTS首次实际编译报两处缺少显式类型与不存在的 `Window.hide()`。按本机声明改为显式类型、`WindowStage.removeStartingWindow()`（API14+）与配套metadata，主窗口不loadContent，顶栏扩展单独loadContent；构建通过。
4. 独立Mac安全审查发现远端按下的键在返回后repeat/up可能漏回本地，以及tap故障回调里同步移除自身/布局UI。已完成独立物理归属账本、返回隔离/仅清理drain tap、preheld本地release安全透传；同步恢复后受generation/connection保护地异步处理teardown/UI。新增126归属策略检查与226真实恢复入口检查，无真实输入调用。
5. Python最终统一回归第一次捕获 `READY+ENTER` 同一recv时假接收器要到下一轮才认证的竞态（1次失败）。已将接收器激活放在处理READY的同一帧回调中，修复而非放宽测试；再次统一验证全部通过。
6. 鸿蒙native整理了完整axis end失败清理、按下失败追踪、无分配release、fd生命周期互斥、session nonce绑定Preferences完成回执、解绑先释放后清凭据。354检查通过，另一次ASan+UBSan同样354通过、无诊断。

### 实际命令与验证边界

```sh
python3 tools/generate_keymap.py --check
python3 -m unittest discover -s tools/mock-peer -v
bash apps/macos/scripts/verify.sh
(cd apps/harmonyos && bash scripts/verify.sh)
python3 tools/test_native_interop.py
python3 tools/verify.py          # 统一回归，写critique/build-test.json与日志
```

- Mac：实际生产Swift/AppKit/Network/Security源码，arm64与x86_64，target14.0，ad-hoc签名；644 Swift检查 + 8真实localhost TCP场景。真实Xcode工程存在并通过plist lint，但没有将lint当成xcodebuild。
- 鸿蒙：实际DevEco native+ArkTS+资源+HAP打包已通过。354项host C++检查链接录制用假OH_Input函数，不链接系统输入服务，另ASan/UBSan通过。`SignHap`任务运行但没有signingConfig，产物明确是unsigned。
- 父会话新增真实Swift Network/TCP/配对状态机 ↔ 真实鸿蒙C++ Client的localhost互通检查：只有客户端单边确认仍unbound，双确认后内存落盘屏障/COMMIT ACK/READY一致，无任何物理注入或磁盘凭据。
- 图形应用未启动，测试未创建活动eventtap、未调用真实光标变更/物理状态查询，未写真实Keychain、未安装鸿蒙应用或触发设备注入。
- `security find-identity -v -p codesigning`为0个有效Mac签名身份，因此不声称Developer ID/公证。鸿蒙工程没有相应调试签名材料，不复制其他应用profile、不伪造自签安装成功。
- 只读hdc事实：在线目标曾为 `192.168.31.140:44491`，`MOR-M1 / 2in1 / OpenHarmony-7.0.0.109 / API26`；存在设备不是已安装/注入通过，更不是API20最低版本通过。
- 动态符号审计实际arm64 `libhop.so`：OH_Input导入只有注入、授权、CapsLock状态查询及事件构造/销毁/setters，没有输入监听/拦截/全局捕获或无障碍符号。

### 首次交付统一回归（2026-11-08 15:41–15:42 +08:00）

`python3 tools/verify.py` 最终exit0，六项子命令均exit0；报告 [critique/build-test.json](critique/build-test.json) 显示 `build.passing=true`、`tests.passing=true`、`failures=[]`，同时保留 `runtimeInputVerified=false` 与 `deviceInstalled=false`。40 Python测试、644 Swift检查、8个真实TCP场景、354 C++检查及Swift↔C++互通全部通过。最后一次鸿蒙构建包含解绑顺序补丁。

首次交付包快照与SHA-256（重新构建/打包后校验和可能变化；之后运行统一验证会自动更新 [artifacts/SHA256SUMS](artifacts/SHA256SUMS) 与 [报告](critique/build-test.json) 的 `artifacts` 字段，校验最新包应以这两处为准）：

- Mac通用ad-hoc ZIP：`apps/macos/build/Hop-macos-universal-adhoc.zip`，`9aa296f90c9a5b2ff68c4a441498958570afaa94d588431654338e2fcd5b7d22`。
- 鸿蒙arm64 unsigned HAP：`apps/harmonyos/entry/build/default/outputs/default/entry-default-unsigned.hap`，1,608,629字节，`97e501108da0f9df20a9d90057113aec769e3f3f109838da6dc62943656d3264`。

包状态：Mac未公证、无Developer ID；鸿蒙没有签名profile，不能直接当可安装签名包使用。构建通过未抹掉上表严格出口缺口。

### 提交口径

仓库最初没有.git，现已初始化。已建立可复查源码提交：`6bf2b6d`（协议/映射/参考测试）、`56c838a`（Mac实现与离线验证）、`b7210cf`（鸿蒙实现与unsigned构建）；最后将验收文档、跨原生互通工具、统一回归报告及包校验和一并提交。**这些不是伪造M0–M4完成的里程碑提交**。待严格出口真正通过后，再按要求分别做对应里程碑验收提交。未签名包、缺少完整Xcode和未进行真机测试均保留公开记录。

### 自动续推进第1轮（2026-11-08 15:51–15:53 +08:00）

- 从当前仓库/goal重新核实：工作目录正确，起始提交 `92e18e3`、干净工作区；本机仍只有CLT，常见位置无完整Xcode；鸿蒙 `build-profile.json5` 的 `signingConfigs` 仍为空，真实Hvigor继续明确警告 `No signingConfig found`。同一外部未满足条件在本自动轮次首次复核，尚不将goal标为blocked或complete。
- 扩展测试专用Swift headless peer，支持左/右布局与显式输入命令；仍只链接原生产codec/TCP/pairing，不创建AppKit应用或InputCapture。真实C++ Client+Injector通过录制用假SDK记录注入；不链接系统输入服务。
- 两个布局均验证：单边确认不落盘、双边确认/COMMIT ACK/READY、带预按Shift/左按钮的ENTER精确ACK、拖动与双按钮、双轴滚轮方向、A重复、右Control、CapsLock，以及边缘返回前先释放Meta+B/按钮并恢复Caps。测试在LEAVING与LOCAL当时检查空集合，不能靠最后Stop掩盖缺失释放。
- 新增 [Xcode结构检查](tools/check_xcode_project.py)：原生plutil解析40对象/53引用，核对全部10个Swift文件（含InputOwnership）、资源生成/复制、Info/最低版本/架构意图与scheme。新增 [17项正反测试](tools/test_xcode_project.py)（16个失败探针+真实工程正例），不修改工程、不代替xcodebuild。Debug仅当前架构，Release架构宏不由本工具求值，共享scheme没有TestAction，均明确记录。
- 根统一验证增加这两项，并在全部成功打包后自动刷新包字节数/签名状态/SHA-256到报告及SHA256SUMS；修复legacy score仅构建通过时应为3的口径。最新一次8项子命令全部exit0，`failures=[]`，两项passing信号为true；40协议测试、17工程测试、644 Swift检查、8 TCP场景、354 C++检查、2个跨原生输入/返回场景全通过。
- 最新包校验已由 `shasum -a 256 -c artifacts/SHA256SUMS` 验证通过。报告保留 `runtimeInputVerified=false`、`deviceInstalled=false`；没有真实权限/图形/Keychain写入、设备安装或物理注入。
- 未满足的严格出口不变：缺少完整Xcode和本工程/目标设备的合法鸿蒙调试签名，真实系统授权/双机输入与故障验收尚未进行。没有用假SDK、静态工程检查或unsigned HAP替代这些出口，goal继续active。

## 已知待实测项

完整Xcode构建、Mac实际权限/Keychain/图形形态/物理捕获、Intel/macOS14运行、鸿蒙有效调试签名与安装、注入授权与撤销、真实移动/拖拽/全键位、左右边缘穿越、顶栏通知与长时保活、急停/断线/崩溃/SIGKILL恢复。退出/权限或tap丢失时不能保证继续过滤已在远端按住的物理尾部，CapsLock全局锁存与系统保留输入也须实测，不能靠假事件单测保证。
