# 双机真机验收清单 / Two-device acceptance

**所有空框留给实际操作的人勾选；离线测试不代替实测。** 本表不预勾任何真机结果。

## 记录测试环境

| 项目 | 实际值 / 备注 |
|---|---|
| 测试日期 / 测试人 | |
| Mac 型号 / CPU / macOS | |
| Mac 构建版本 / 签名身份 | |
| HarmonyOS PC 型号 / 系统版本 / API | |
| HAP 构建版本 / 签名 profile 有效期 | |
| LAN / 有线或 Wi-Fi / 左右方向 | |
| 鼠标型号 / 键盘 ANSI、ISO 或 JIS / 输入法 | |

开发环境只读探测曾发现 `MOR-M1 / 2in1 / OpenHarmony-7.0.0.109 / API26`，这**不等于 HarmonyOS6/API20 已验收**。须补目标最低系统版本测试。

## 安全准备（先做）

- [ ] 两台设备均可本地直接操作，有备用物理鼠标/触控板与键盘；不要在有未保存工作的应用中首次测试。
- [ ] 两端在同一可信局域网，没有公网端口转发；知悉 v1 明文与 token 无法防窃听/主动篡改。
- [ ] Mac 默认关闭共享，能看到菜单栏 Hop 图标；鸿蒙只有顶栏图标、状态小面板与合法常驻通知。
- [ ] 学会急停：Mac 上 **Ctrl + Option + Command + Esc**；先在共享关闭时熟悉操作位置。
- [ ] 记录故障退出方式；初次穿越前保留终端可退出 Hop，确认远端没有敏感文档聚焦。

## M0 — 工程与安装基础

- [ ] 完整 Xcode 打开工程并成功 Build（不能用 CLT 编译结果替代此项）。
- [ ] Apple Silicon Mac 上运行成功；macOS14最低版本另行验证。
- [ ] Intel Mac 上运行成功（交叉编译不能替代此项）。
- [ ] DevEco 打开工程并成功 assembleHap；最低 API20兼容配置正确。
- [ ] 为**本机设备 UDID**签发调试 profile，签名 HAP 成功，开发者模式/调试连接安装成功。
- [ ] 两端包身份固定；重新构建/签名后的权限行为明确；没有误装另一项目的签名 profile。

## M1 — 手填 IP 与首次真实注入

- [ ] 使用手填 Mac 局域网 IP 的开发入口连接，不使用公网；TCP24890连通。
- [ ] 鸿蒙先出现系统注入授权弹窗，拒绝时不能注入、Mac仍可本地操作。
- [ ] 授权同意后成功注入鼠标到正确全局像素位置；不是仅 UI 显示“已连接”。
- [ ] 没有授权、授权撤销、其他应用占有授权、系统801不支持等情况显示真实错误且释放/断开。
- [ ] 同一输入会话没有请求全局捕获权限、CONTROL_DEVICE、无障碍或其他旁路。

## M2 — 鼠标和双向边缘穿越

分别按「鸿蒙在右」与「鸿蒙在左」各完整执行一次：

- [ ] 右布局：Mac右缘进入鸿蒙左侧内缘；垂直位置按比例保持。
- [ ] 左布局：Mac左缘进入鸿蒙右侧内缘；垂直位置按比例保持。
- [ ] 远端未确认 ENTER、网络延迟或 ACK 超时，Mac不隐藏/冻结本地光标，不吞本地输入。
- [ ] ENTER等待期间离开边缘、切换按钮/修饰键、按非修饰键，取消进入且无本地失控。
- [ ] 进入后需向内移动才激活返回边缘，不在入边界立即反复弹回。
- [ ] 返回反向边缘，Mac本地光标恢复在边缘内侧，不立刻再次穿越。
- [ ] 远端移动平滑，无无限积压；不同 DPI、Retina与鸿蒙缩放下坐标/速度合理。
- [ ] 左/右/中/前进/后退按钮：逐项按下/松开、连续点击、双击。
- [ ] 在鸿蒙本机应用中拖动窗口、选择文字、拖动图标，松开无粘滞；不测试跨机文件拖拽。
- [ ] 按住鼠标从Mac进入鸿蒙，远端按下状态正确；按住按钮返回时两端均无卡住或幻影点击。
- [ ] 多按钮同时保持与移动（SDK没有pressed-buttons数组 setter，尤其要验证真实系统行为）。
- [ ] 垂直与水平滚轮方向、连续滚动和自然滚动设置各自正确；不把触控板手势当协议支持项。

## M3 — 键盘物理键位与组合

按 `docs/KEYMAP.md` 对每个实际硬件支持键记录结果。Cmd→Meta，不替换为Ctrl；目标布局/输入法负责产生字符。

- [ ] ANSI字母26个、数字10个、空格/回车/Tab/退格/Esc、全部标点逐个检查。
- [ ] 左右 Shift/Ctrl/Option/Cmd 分别单独检查，均正确down/up，不混淆左右侧。
- [ ] Shift+字母、Ctrl+字母、Alt+字母、Meta+字母，以及2–3个修饰键叠加无遗留。
- [ ] CapsLock在进入前开/关、远程中切换、退出时状态符合锁定语义，不是永久按住。
- [ ] 箭头、Home/End（导航，不是系统Home）、PageUp/Down、ForwardDelete、Help/Menu。
- [ ] F1–F20、数字小键盘及Enter/Equals/Comma（硬件有则测，没有标N/A）。
- [ ] ISO额外键、JIS Yen/RO/小键盘逗号（对应布局硬件有则测）。
- [ ] 重复按键、长按、快速按下松开、输入法切换、远端组合键对本地无误触发。
- [ ] 非修饰键已在Mac按住时，不强行穿越造成缺少远端keydown。
- [ ] Fn、Keypad Clear、JIS英数/かな明确不承诺映射；系统媒体/亮度/Touch ID不冒充已支持。
- [ ] 在远端按住每一种修饰键断网/急停，各键被释放，不留下锁住的修饰键或连发。

## M4 — 配对 / 重连 / 顶栏 / 故障恢复

### 配对与状态

- [ ] Bonjour `_hop._tcp` 发现列表可见Mac，无手填情况下可选择设备。
- [ ] 配对仅显示于两端顶栏小面板，六位码一致；只确认一端时不得获得可用输入会话。
- [ ] 确认两端后token持久化：Mac Keychain、鸿蒙Preferences；重启进程后安全重连。
- [ ] 错误token/UUID/nonce/码被拒绝，不自动降级成无确认新配对。
- [ ] COMMIT后断网的半配对可恢复，超过候选有效期/服务端重启时有明确重新配对流程。
- [ ] 绑定状态不接受陌生设备/第二台客户端抢占；非配对窗口的新配对被拒绝。
- [ ] 在线解绑双方清理凭据；离线解绑后重连被拒绝并引导另一端手动清理。
- [ ] 自动重连仅在已有绑定且用户没有主动断开的情况下进行，退避不忙循环。
- [ ] 主动断开后不立即悄悄重连；再次选择/连接可恢复。

### 形态与后台

- [ ] Mac无Dock主窗口，仅菜单栏；Popover只含约定项目及必要授权/配对确认。
- [ ] 鸿蒙启动后无常驻主窗口；顶栏图标点击只打开小面板，关闭面板连接不断。
- [ ] 长时任务合法启动并显示系统常驻通知，点击通知回到顶栏操作流程。
- [ ] 面板发现列表/配对确认/连接状态/断开均可操作；不依赖隐藏设置大窗口。
- [ ] 后台闲置30分钟、熄屏/睡眠唤醒、断网恢复时真实连接状态清晰，资源占用合理。
- [ ] 无辅助功能或输入监控授权时Mac在Popover引导系统设置，未授权绝不冻结光标。

### 每条异常的第一优先级：本地键鼠立即恢复

每次均在远端模式、按住按钮或修饰键下测试，并记录恢复时延/现象：

- [ ] Ctrl+Opt+Cmd+Esc急停：立即恢复，不等TCP，不把急停组合发送为远端操作。
- [ ] 点击Mac禁用开关/退出；鸿蒙主动断开/退出/授权撤销。
- [ ] 拔网线、关Wi-Fi、对端进程崩溃、静默3秒无心跳。
- [ ] 恶意超长帧、非法版本/保留字节、旧epoch/越权输入，恢复后安全断开。
- [ ] EventTap被系统禁用、辅助功能/输入监控授权撤销、显示器分辨率变化。
- [ ] Mac睡眠/锁屏/唤醒；不支持登录前与安全桌面注入，不旁路系统限制。
- [ ] 慢写/发送队列溢出/断连与ENTER ACK竞态，不会持续吞本地事件。
- [ ] **SIGKILL/崩溃清理**：确认OS回收eventtap与鼠标关联/隐藏状态；此项没有finally可运行，不能仅靠单测认定安全。
- [ ] 重复恢复/重复断开幂等，光标不额外隐藏，不产生本地合成点击/键盘事件。

## M5 — 最终记录

- [ ] 中英README与安装说明可由未参与开发的人照做；包的签名/未公证状态准确。
- [ ] 分开列出“构建通过”“离线通过”“真机通过”；不将API26运行推广为API20实测。
- [ ] 每个实际完成的里程碑有git提交，不能把缺少实测的源码提交称为M1–M4通过。
- [ ] 下方记录所有失败/限制，保留可复现步骤；所有必要项通过后才称可日用。

## 实测失败 / N/A / 待处理记录

| 项目编号或描述 | 通过/失败/N/A | 设备与现象 | 复现步骤 / 日志 / 后续 |
|---|---|---|---|
| | | | |

## English summary

Check every item on two real computers. Start with a fallback mouse/keyboard and emergency shortcut. Verify actual authorization and injection, both crossing directions, all supported physical keys/modifiers, drags/wheels, mutual pairing, persistence, reconnect/unpair, tray-only UI and continuous-task notification. Test every recovery path, especially SIGKILL cleanup. Build, mock, localhost and fake-cursor tests are separate evidence, never a substitute for these unchecked runtime results. Record exact OS/API/signing versions and failures; unsupported hardware keys must remain explicit rather than being marked passed.
