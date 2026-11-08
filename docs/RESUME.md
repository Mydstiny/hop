# 恢复真实双机验收的最短路径 / Resume real-device acceptance

## 当前事实，不是确认表单

本工程已完成可复现的原生构建、协议/安全逻辑离线验证和左右跨原生回环；**未完成严格M0–M4真机出口**。SDK输入API红线检查通过。当前已验证与仍待本人操作的条件如下：

1. 本机Xcode27.2首次设置已经通过，真实Release工程构建exit0，双架构/最低14.0/LSUIElement=true已核对。原CLT仍可单次指定使用，不需修改全局选择。
2. 用户本机签名已绑定，signed HAP官方验签通过、dev.hop.input/最低API20正确；MOR-M1/2in1/API26 PC已重连、安装和启动成功，Mac产品也已启动。还没有代理点击授权、配对确认或开启捕获，真实输入/故障验收仍未进行。
3. 系统权限、真实输入、菜单栏/顶栏与保活、断线/急停/SIGKILL及最低版本仍需按 [未勾选验收清单](<ACCEPTANCE.md>) 验证。

本机已完成下面的工具/签名准备步骤；换机或重装时仍需按步骤复核。当前重点是系统授权、两端核对配对码，然后按M1开始物理验收。

不需要把开发者密码、私钥、证书包、profile或UDID发到聊天或提交仓库。不要以别的应用签名、改名unsigned包、关闭签名/Gatekeeper检查或无障碍旁路替代这些条件。

## 1. 本地先复现已有验证

从仓库根目录运行：

```sh
python3 tools/verify.py
```

它实际构建Mac通用原生app和鸿蒙HAP（有本机有效签名配置时还生成并官方验签signed包），并运行协议/工程/预检安全测试、Swift恢复测试、C++录制假SDK测试及左右跨原生回环；不会安装/启动产品应用或捕获/注入物理输入。最新结果见 [构建报告](<../critique/build-test.json>)、[包校验和](<../artifacts/SHA256SUMS>)。

```sh
shasum -a 256 -c artifacts/SHA256SUMS
```

有WindowServer会话的Mac还可显式运行 `python3 tools/verify.py --include-native-ui` 或 `bash apps/macos/scripts/ui-test.sh`，验证真实AppKit内容视图的284项布局/动作检查，生成5个离屏假数据PNG。它创建测试NSApplication，但不显示窗口、不实例化产品捕获/网络/存储控制器、不查询/请求真实输入权限。默认总入口不启用此测试，并记录跳过理由。它也不是实际Hop启动/菜单栏弹窗或真机截图验收。

只有签名/物理验收以后才能把相应真实出口勾成通过，不能因为这些离线/离屏验证全绿而勾选。

## 2. 补齐完整Xcode，不改全局工具选择

通过Apple官方渠道自行安装完整Xcode，完成其首次启动/许可/组件设置。安装位置若为常见路径，可只为本次命令指定：

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  bash apps/macos/scripts/xcode-build.sh
```

这才是真正的Xcode工程Release构建；plist lint/结构检查/CLT编译不能代替它。保持最低macOS14.0、arm64+x86_64、固定包名和LSUIElement。完整步骤见 [Mac安装与签名](<INSTALL_MAC.md>)。

## 3. 在你本机配置合法鸿蒙签名

用DevEco自己的自动调试签名流程与自己的开发者身份，选择本工程 `dev.hop.input` 和准确的已授权PC/2-in-1设备；或使用你自己签发的匹配调试证书/profile。重新构建，确认产物真实签名并通过官方验签；不要上传签名材料。

签名配置可能改动已跟踪的工程配置文件。不要无差别 `git add .`，更不要提交密码或签名材料；完成验收后的源码提交只应包含脱敏、可公开的配置与真实验证结果。完整步骤与有效期/UDID注意事项见 [鸿蒙调试签名教程](<INSTALL_HARMONYOS.md>)。

## 4. 精确选择设备并只读预检

```sh
/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc list targets
python3 tools/device_preflight.py --target YOUR_DEVICE_TARGET \
  --hap /ABSOLUTE/PATH/entry-default-signed.hap --require-prerequisites
```

多设备时必须指定目标，预检不猜测、不自动连接/配对。当前已只读确认的PC曾为MOR-M1/2in1/API26；无线地址和端口可能改变，不要仅凭旧地址安装。

预检严格模式exit2表示条件仍缺，exit3表示已读真实SDK的必需API声明缺失/签名不同；默认exit0仅代表报告已收集。SDK路径不可读只报环境未知，不冒称API不存在。最新现场事实见 [只读预检报告](<../critique/device-preflight.json>)。

官方验签exit0也不保证该设备允许安装：仍须核对证书信任、应用包名、profile设备资格/有效期。验证器按官方要求产生的证书链/profile输出与日志仅在本地git忽略的私有产物目录，不要公开。

## 5. 先最小M1，再逐项推进M2–M4

- 保留直接连接Mac的备用键鼠，记住 **Ctrl+Option+Command+Esc** 急停。
- 按两端安装教程启动你确认来源的应用；Mac默认共享关闭。确认无常驻主窗口、系统通知/合法后台任务和输入权限正确。
- 首次只做可信LAN手填IP、双方核对并确认配对码、实际鼠标移动；不要在终端或有破坏性操作的应用中试键盘。
- 然后左右布局分别验收穿越/返回、按钮/拖拽/滚轮、逐键与组合键、断网/权限撤销/急停/退出/崩溃/SIGKILL。不要用录制假SDK记录代替系统实际效果。
- 记录两机型号、OS/API、签名状态、准确动作与结果；相应严格出口真正通过后再作对应里程碑提交。API26不代表API20运行通过，arm64构建也不代表Intel运行通过。

## English

The real Xcode27.2 Release project build and signed HarmonyOS debug package are now verified; the confirmed MOR-M1 PC accepted installation and ability startup, and the Mac host process is running. Remaining gates are legitimate system grants, mutual code confirmation and the unchecked physical acceptance checklist. Do not send or commit credentials/UDIDs, borrow profiles, rename unsigned output or bypass trust/authorization. Run the combined offline regression first; use a per-command `DEVELOPER_DIR` for Xcode, sign locally in DevEco, select the exact PC target and run the read-only preflight on the actual signed artifact. Default preflight exit0 is report collection only; strict exit2 means missing prerequisites. Even SDK signature verification is not device-profile eligibility or physical input proof. Start with manual-IP M1 and a fallback local keyboard/mouse, then record and commit each genuinely passed milestone.
