# M1 首次双机操作卡 / First physical mouse test

这不是确认表单，也不是已通过的验收结果。构建/签名/安装已自动推进；系统授权和双方核码必须由操作者本人完成。禁止用权限数据库、调试器、伪造token或UI自动确认绕过。

## 已核实的起点

- Mac应用身份 `local.hop.macos`，实际LaunchServices activationPolicy=accessory，进程完成启动，TCP24890正在监听。
- Mac到PC的本次路由接口为 **en1**，Mac局域网IPv4快照为 **192.168.31.186**。地址可能变化，以下命令产生的最新[运行检查报告](<../critique/runtime-readiness.json>)优先。
- 接收端 `dev.hop.input` 的初版曾在MOR-M1 / 2in1 / API26安装启动，18:28进程存活；19:51复核已没有该进程。用户报告顶栏/面板异常后，冷启动捕获到图标尺寸和 `Ans: null uiContent` 错误。修复版已官方验签并安装，最新正常启动被PC锁屏10106102拒绝；必须本人先解锁，尚未验证新版图标/任务/通知。不是手机/平板目标。
- 当前报告没有观察到Mac TCP24890的ESTABLISHED会话；这不证明具体哪个权限/配对步骤缺失，也不能据此冒称注入故障。
- accessory/进程/监听不等于“无主窗口已验收”、常驻通知有效、授权成功、配对成功或实际输入成功。

## 实际操作（只先试鼠标）

1. **安全准备**：两端同一可信LAN；保留直接操作Mac的键鼠/触控板；不要聚焦终端、密码框或未保存文档。记住 **Ctrl+Option+Command+Esc** 急停。v1是明文TCP+token，不能用于公共网络。
2. **解锁并启动鸿蒙Hop**：先本人解锁PC并保持亮屏，从启动器正常打开Hop（或让开发工具正常启动）。10106102表示系统锁屏拒绝启动，即使hdc表面exit0也不算成功；不要关闭开发者模式或绕过锁屏。点顶栏H打开小面板。本版不再从没有UIContent的主Ability请求普通通知弹窗，而由合法TASK_KEEPING API产生系统持续任务通知，并查询自有活动任务；它失败时连接仍禁用。若报告9800006，检查系统设置中Hop的通知许可，再本人正常重启Hop，不伪装音频、不关闭全局安全检查、不靠进程存活判定成功。
3. **鸿蒙授权**：点 `Authorize`，本人处理系统注入授权弹窗；应看到 `System injection authorized`。拒绝授权时不应建立可用注入会话，Mac本地键鼠仍应可用。不要给鸿蒙配置全局捕获、CONTROL_DEVICE或无障碍旁路。
4. **Mac菜单栏Hop**：打开小面板，按引导允许Hop的辅助功能、输入监控以及必要的本地网络访问；系统若要求重启Hop，先正常退出再打开。不要关闭全局Gatekeeper/防火墙。先保持 `Enable sharing` 关闭，选择实际左右方向。
5. **配对窗口**：Mac点 `Pair a computer…`。鸿蒙在 `Mac LAN IP (manual fallback)` 填最新Mac地址（本次为 `192.168.31.186`），点 `Pair using this IP`。该IP是Mac，不是PC的调试地址，端口也不是hdc端口。
6. **本人核码**：在两个面板检查同一个六位码，双方分别点 `Codes match — Confirm` / `Codes match — confirm`。只确认一端不能形成可用输入会话；超时则重新打开配对窗口。不要把码或token发到聊天。
7. **最小鼠标验证**：在Mac启用共享，轻推鼠标到选定边缘，确认PC实际指针移动到正确位置；向PC内侧移动后再推反向边缘返回Mac。先不要打字、拖文件或测试破坏性快捷键。
8. **结果记录**：明确记录“PC指针确实移动/不能移动”“能否返回”“急停是否恢复”。只看到Connected或TCP连接不算实际注入通过。通过后再逐项做[完整未勾选清单](<ACCEPTANCE.md>)，不能把API26结果推广为API20实测。

## 不打扰正在运行的产品的只读检查

```sh
python3 -B tools/test_mac_runtime_probe.py --target 192.168.31.140:44491
```

此命令只在忽略的测试目录编译独立Swift探针，运行4项参数/身份/路径/真实元数据检查；然后读自有进程、监听和本机路由。不会重编译或重启产品、不会占用单客户端的产品TCP端口，也不读Keychain/Preferences、截图、授权、确认配对或捕获/注入。多目标不猜设备，先核对指定PC类型/API；无显式target不查询设备。测试exit0只表示这些探针测试通过，绝不表示M1验收成功。

产品正在等待真实操作时，不为刷新报告反复构建/签名其原路径，以免干扰已运行的可执行文件或权限身份。已有完整产品回归见[构建报告](<../critique/build-test.json>)，本探针证据单列，不能覆盖成新的完整产品构建成功。

本次尝试的设备工具 `bgtaskmgr -h` 不存在（hdc表面exit0但正文not found），不把它当后台任务成功；公开aa命令对指定Hop扩展返回无记录，也不证明图标不存在。通知、面板与授权状态仍须看本人设备上的实际UI，未采集其他应用窗口/全局任务列表。

## English

The Mac is running as an accessory app and listening on TCP24890; the confirmed PC receiver is alive. Use the latest report's Mac LAN IPv4 (snapshot192.168.31.186), not the PC debug endpoint. The operator must allow legitimate system permissions, open the Mac pairing window, compare the six-digit code on both panels and confirm both, then enable sharing for the first real mouse crossing/return test. No permission, pairing confirmation or input action is automated by this probe. Process/socket metadata and its four tests are not physical acceptance; keep the full checklist unchecked until actually verified.
