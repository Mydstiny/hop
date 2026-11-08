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

### 自动续推进第2轮（2026-11-08 16:00–16:01 +08:00）

- 重新核对起点 `a77afe1`、干净工作区、goal第2轮active。按用户要求后续主线程串行推进，不再批量创建agent。
- 新增 [只读预检](tools/device_preflight.py) 与 [25项安全单测](tools/test_device_preflight.py)。检查SDK真实函数/异步回调/枚举与API20 availability标注（不等同API20真机运行），区分“SDK路径/头文件未知”和“已读真实声明缺失/不匹配”，避免把环境问题冒称API红线。实际检查 `redlineHit=false`，输入库与HMS桌面kit存在。
- 配置使用本机vendor JSON5解析器，仅输出计数/版本/是否选中配置，不输出密码或材料。HAP身份/最低版本从实际module.json读取，并调用官方帮助确认的 `verify-app -inFile ... -outCertChain ... -outProfile ...`。官方进程exit1，明确 `No Hap Signing Block before ZIP Central Directory`，证实实际HAP无签名块；一般验证器错误另标unverified，不误称unsigned。证书/profile验证输出与日志仅留在git忽略的私有产物目录，不读取UDID/profile内容或私钥，也不改名伪造signed包。
- 本机当前有3个hdc目标；默认预检拒绝猜测。随后显式选择此前确认的 `192.168.31.140:44491`，只读参数成功复核 `MOR-M1 / 2in1 / API26 / OpenHarmony-7.0.0.109`。未查询其他目标详情，未读取UDID，未安装/启动、配对、弹权限或注入。
- [实际预检报告](critique/device-preflight.json) 的 `automatedPrerequisitesSatisfied=false`；严格模式进程exit2（这是条件未满足，不是验收通过），仍缺完整Xcode、当前工程合法调试签名配置与有效签名HAP。即使未来SDK验签exit0，也保留profile设备资格/系统授权/实际Xcode构建与全部物理验收待人工验证。
- 根统一验证加入预检单测，9项命令全部exit0、两项passing信号为true、`failures=[]`。总计82项Python单测（协议40+工程17+预检25）、644 Swift检查、8个TCP场景、354 C++检查、左右2个跨原生输入/返回场景通过。未改变应用功能/范围；Mac仍ad-hoc，鸿蒙仍unsigned。
- 第1轮已记录的同一外部条件（缺完整Xcode与本工程/目标设备的合法签名）在第2轮连续复核后仍存在。尚未达到完整目标，不标complete；本轮继续保留active，不提前标blocked。最新包校验与构建报告一并保留。

### 自动续推进第3轮与收尾（2026-11-08 16:03–16:15 +08:00）

- 将生产Popover内容拆到小型presentation/action协议边界，正常产品仍读取AppController真实状态；新增真实AppKit内容视图的284项离屏检查和5个内存假数据PNG，不实例化产品网络/存储/捕获控制器，不读取权限、不创建可见窗口。实际菜单栏生命周期/真机输入不在此测试范围。
- 首次新测exit1：34个“横向越界”来自把NSTextField装饰frame误当Auto Layout alignment rect。读SDK的NSLayoutConstraint契约并检查配对/长文案PNG后，改为同时核对alignment margins与实际frame边界，并新增确实越界的反例；未修改生产布局、未忽略失败。修正后独立和完整回归均284项通过。
- `python3 tools/verify.py --include-native-ui` 的10项命令exit0；最新结构化报告记录可选UI测试的明确范围，默认不启用并说明跳过原因。新增[恢复指南](<docs/RESUME.md>)；物理验收表仍全部留给真实验收。
- 第3轮严格只读预检仍exit2：同一缺完整Xcode/本工程合法签名条件连续3轮不变，SDK红线未触发。goal曾按这些必要外部条件标blocked而非complete，导致本轮尚未执行Git提交；后续人类指令恢复后统一收尾，不冒称当时已提交。

### 用户续推：Xcode、合法签名与实际部署（2026-11-08 17:42 起，+08:00）

- 确认鸿蒙bundleName为 `dev.hop.input`。起初仅CLT、磁盘约59GiB、系统/用户Applications及Spotlight无完整Xcode，Downloads无Xcode安装包。网页工具访问Apple官方页面因解析到非公共地址被拒，未绕过抓取限制；使用系统App Store官方Xcode链接的open请求exit0，不冒称这等于下载/安装完成。未读取Apple账户/密码、未调用sudo或全局切换工具命令。
- 新增[完整Xcode发现工具](<tools/resolve_xcode.py>)及11项文件系统/模拟CLI测试；明确拒绝把CLT冒当完整IDE，保留显式覆盖、空格路径、改名应用、正式版优先和单进程DEVELOPER_DIR。工程构建脚本/只读预检复用该发现逻辑，并检查首次设置，不把版本exit0误当许可/组件就绪。预检单测现为26项。
- 用户随后提供本机签名材料。脱敏检查发现1份配置，但default产品未绑定，且IDE将compatible SDK抬到26。仅编辑公开产品字段绑定本机default签名并恢复 `6.0.0(20)`，编译target仍26；未展示、复制其他应用材料或提交用户签名配置/密码/证书/profile。构建报告改为从实际module读取包名/最低API，偏离dev.hop.input/API20即失败；配置签名时要求实际signed产物并用官方SDK验证，不按文件名认定有效。
- 17:54发生真实失败：安装后的全局工具选择已经变为Xcode27.2，首次许可尚未完成，原生C++测试exit69；同期DevEco构建exit0、SignHap成功、官方SDK verify-app exit0，不把测试失败藏成全绿。只为离线子进程使用原CLT（未改全局选择），随后11命令统一回归exit0：94项Python测试（40协议+17工程+26预检+11发现）、644 Swift、8 TCP、354 C++、左右2互通、284 AppKit。原始记录在忽略的日志，最新[根报告](<critique/build-test.json>)只代表该次实际成功。
- 已请求打开Xcode首次设置，没有替用户接受许可；随后实际 `-checkFirstLaunchStatus` exit0。17:59:59起真实Release `xcodebuild ... ARCHS='arm64 x86_64' ONLY_ACTIVE_ARCH=NO CODE_SIGNING_ALLOWED=NO build` exit0、BUILD SUCCEEDED；复核产物两种架构、LSUIElement=true、最低14.0。[独立工程构建证据](<critique/xcode-build.json>)区别于离线根入口和静态图检查，不再报告缺完整Xcode。
- 实际signed HAP为1,674,455字节，SHA256 `58f8f10b6932b51903ceb9da1fe06f48059e779e46147430675a3b4550c2e225`，SDK验签exit0、实际minAPIVersion60000020。旧unsigned仍保留作独立构建产物，不改名充签名包。签名输出/private验证文件不纳入Git。
- 调试目标一度仅有MLR-AL10/tablet与SGT-AL10/phone，只读确认后未向它们安装。对先前已授权PC的精确旧端点仅作一次重连，最终Connect OK；未扫描/猜新端口、未读取UDID。重验MOR-M1/2in1/API26；18:02严格预检exit0、无blockingConditions、无API红线。预检不执行工程构建/安装，因此这些动作的证据分别记录，自动前置条件通过不等于物理验收。
- 对同一已验签hash的signed HAP执行该PC的 `hdc install -r`，真实输出 `install bundle successfully`、exit0；随后启动EntryAbility输出 `start ability successfully`，只读确认接收端PID46919。Mac open请求exit0，进程PID77378存活；没有点击授权/配对/共享、没有捕获或注入测试。首次Hilog读取同时使用-x/-z被CLI以CODE -31拒绝（命令表面exit0不作为成功）；按实际帮助修正为仅-x和自有PID/等级过滤，W/E/F当前0行，不据此宣称GUI或后台功能全通过。
- 原先的缺IDE/签名阻碍现已解除，goal保持active、未标complete。当前两端仅启动；需要合法系统授权与两端核码确认后才能进行真正M1输入验收。任何后续勾选必须对应真实输入/故障证据，不以进程、安装、验签或离屏图替代。

### 自动续推进第4轮（2026-11-08 18:14–18:28 +08:00）

- 基于当前工作树/进程而非旧叙述复核：两端原PID仍存活，PC调试端点仍授权。Mac真实内核TCP24890为LISTEN，没有观察到该端口ESTABLISHED会话；这不能定位哪个权限/配对步骤未完成，更不是实际输入通过。
- 新增[只读原生元数据探针](<tools/mac_runtime_probe.swift>)及[独立构建测试入口](<tools/test_mac_runtime_probe.py>)，严格限定local.hop.macos及预期bundle路径。实编译exit0，4项参数/身份/同ID不同路径/真实元数据检查通过；OS实际返回accessory policy=1、finishedLaunching=true、PID77378。明确不观察其他应用窗口、不查TCC/Keychain/Preferences、不授权/核码/启用捕获或注入。
- 新[运行检查报告](<critique/runtime-readiness.json>)与原始日志单列，两个passing信号仅属于该隔离探针，不覆盖原完整产品构建报告。真实复核MOR-M1/2in1/API26和接收端PID46919；本机到PC的路由为en1，手填Mac IPv4为192.168.31.186。只读内核/路由，不向单客户端产品监听端口建立探测连接，不为刷新报告重编译、重签或重启正在等待操作的产品。
- 查实际HMS SDK声明确认addToStatusBar同步重载返回void，现有调用没有误漏await，不制造未经证实的修复。设备 `bgtaskmgr -h` 正文为not found，尽管hdc表面exit0，仍判未知；`aa dump -e dev.hop.input/entry/HopStatusBarAbility` 仅查询自有扩展并返回无记录，不据此推断图标/后台任务不存在。没有采集全局任务、窗口或其他应用数据，也没有API红线证据。
- 新增[首轮鼠标操作卡](<docs/M1_RUN.md>)，写明准确英文按钮、通知/注入授权、Mac隐私设置、本人双方核码及最小穿越/返回动作。当前缺少实际UI和物理操作观察，不把未知状态标成blocked或M1通过；goal继续active，后续需从操作者的界面/鼠标结果推进，而不是重复假注入测试充数。

### 第4轮续：用户报告顶栏/面板异常后的真实诊断（2026-11-08 19:51–20:13 +08:00）

- 用户反馈“鸿蒙找不到H图标，或点不开面板”。重新核实Mac原PID77378仍LISTEN且没有产品TCP会话；MOR-M1/2in1端点仍在线，但旧接收端PID已不在。正常启动一次自己的EntryAbility，得到新PID53122；只采集该PID启动错误，未抓全局窗口/输入/其他应用数据。
- [冷启动证据](<critique/receiver-relaunch.json>)明确显示StatusbarManager像素图尺寸错误、Ans `null uiContent` 及Hop启动失败。实际SDK要求requestEnableNotification必须在UIContent加载后调用；本工程故意没有主窗口内容，旧调用不满足前提，不能用创建主窗口或伪装context解决。
- 修复[EntryAbility](<apps/harmonyos/entry/src/main/ets/entryability/EntryAbility.ets>)：图标24vp按真实display density转像素；用公开callback重载包装Promise，验证注册而非盲认为void调用成功；去掉空主Ability通知弹窗请求，仍由合法TASK_KEEPING产生系统持续任务通知，失败禁连。通过API20 getAllContinuousTasks核对自有活动任务/通知ID，不dump全局任务。增加具体失败阶段/错误码；moveAbilityToBackground失败也必须释放starting锁，用户正常重新激活可重试而不重复注册图标。未动Mac产品、输入授权或配对确认。
- 鸿蒙独立verify通过：原生假SDK354项、实际ArkTS/原生/HAP构建、官方验签均exit0；signed包1682940字节，SHA256 `8ca36342f2f9e2b82c063dc0f30a74191fcd27d59a8d72a1c0769987f860aa15`，包名dev.hop.input，实际最低60000020→API20。PC真实安装成功；正常启动却被**锁屏10106102**拒绝，hdc外层仍exit0，按正文判失败。之后一次正常重试仍锁屏、无PID；[部署记录](<critique/receiver-startup-fix.json>)明确appStarted/tray/task/runtimeFix均false，不能称图标已修好。已提醒本人解锁，不执行CLI错误建议的关闭开发者模式，也不绕过锁屏/输入授权。
- 在等待本人解锁时补齐完整回归而不打扰Mac：原脚本已支持HOP_BUILD_DIR，修复总验证器必须哈希该实际输出而非旧默认ZIP，并拒绝已变更HAP沿用旧SDK验签标签；新增6项合成元数据回归，明确不是验签/真机测试。隔离目录执行总入口含native-ui，**12条命令全部exit0，100项Python、644项Swift、354项C++、8 TCP、2跨原生回环、284离屏AppKit检查通过**。Mac原PID/角色/启动状态和可执行文件SHA256前后不变，见[保留运行实例证据](<critique/mac-live-preservation.json>)；新包路径按实际隔离目录记录，HAP摘要不变。
- 20:13对当前8ca产物重新执行严格只读预检exit0、SDK红线false、min20/签名通过、目标仍MOR-M1/2in1/API26；随后三条实际产物SHA256校验全OK。该预检不查询锁屏/授权，不得用前置条件passing覆盖正常启动锁屏失败。
- 更新SDK审计、首次操作卡、安装/恢复说明和中英README，清除残留“本机没有完整Xcode”的旧句。真实新顶栏/持续任务通知与输入验收仍待解锁后观察，清单不勾选。锁屏是在本轮新确认的安全前提，不伪造成功或把两次同轮重试当作三轮阻塞。

### 第4轮续：本人解锁后的真实启动结果（2026-11-08 21:17 +08:00）

- 操作者明确已解锁并请求继续；再次核对MOR-M1/2in1后正常启动，无重装/重编译。aa正文成功，当前自有接收端PID47722；仅查该PID的Hop标签，不读取输入、截图、全局窗口或其他应用。
- 实际图标尺寸46px，顶栏注册callback成功；合法TASK_KEEPING请求后公开API查询得到自有活动task55/notification36、mode taskKeeping，见[累计部署证据](<critique/receiver-startup-fix.json>)。这是实际系统结果，不是录制假SDK。
- 仍有moveAbilityToBackground Operation-not-supported警告；已被finally处理，未锁死startup，也没有创建主窗口兜底。不能从API成功推导图标可见、面板能打开或无主窗口；这些字段继续false，需本人视觉观察。另发现hilog `%{public}f` 未正确打印密度（像素尺寸已正常记录），仅诊断格式问题，不为此打断运行中的产品重部署。
- 未代按Authorize/核码/开启共享，实际注入与后续M1–M4清单仍未通过。下一步先确认H图标/面板及无主窗口，再本人授权与物理鼠标，不用API回调冒充视觉或输入验收。

### 第4轮续：操作者确认顶栏UI（2026-11-08 21:30 +08:00）

- 操作者明确反馈“H出现，面板能打开，没有主窗口”；该单项视觉证据按operator_reported记录，不冒称代理截图/全局窗口枚举。与21:17真实注册回调及活动taskKeeping任务55/通知36结合，可确认本次启动UI修复，不据此确认通知外观、真实授权或输入，更不勾完复合M0–M4清单。
- 只读重新核实原Mac进程77378及TCP24890监听、PC进程47722仍存活；当前路由接口en1，Mac手填IPv4仍192.168.31.186，没有已建立的产品TCP会话。没有自动点击Authorize、核码、启用捕获或制造输入。
- 下一步一次按操作卡完成本人系统授权、Mac配对窗口、PC手填Mac地址、双方核码，然后Mac启用共享，仅试鼠标穿越/内侧移动/返回。遇到阶段错误先收集准确提示，不能用“Connected”或进程存活代替实际指针移动。

## 已知待实测项

Mac实际权限/Keychain/图形形态/物理捕获、Intel/macOS14运行、鸿蒙注入授权与撤销、真实移动/拖拽/全键位、左右边缘穿越、顶栏通知与长时保活、急停/断线/崩溃/SIGKILL恢复。退出/权限或tap丢失时不能保证继续过滤已在远端按住的物理尾部，CapsLock全局锁存与系统保留输入也须实测，不能靠假事件单测保证。
