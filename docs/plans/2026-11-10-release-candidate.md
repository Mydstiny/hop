# Hop 2.0.0-rc.1 · 构建、验证和交付清单

本文记录首批四个本地 RC1 候选包的构建和验收背景。GitHub 上的三端预览包及签名状态以 [README](../../README.md) 和发布说明为准。Mac 与鸿蒙的跨屏回程另有 [四向测试](../ACCEPTANCE_UI2.md)，v1 实测见 [v1 说明](../../README.v1.md)。

[四平台计划](2026-11-09-cross-platform-product-plan.md) · [v2 协议](../V2_SESSION.md)

## 安装包范围

| 系统 | 候选包 | 安装与签名要求 |
| --- | --- | --- |
| macOS 14+ | `Hop-2.0.0-rc.1-macos-universal-adhoc.zip`，arm64 + x86_64 | 本地 ad-hoc 候选；正式下载版还须 Developer ID Application 签名及公证。首次运行须授予辅助功能和输入监控。 |
| Windows x64 | `Hop-Setup-2.0.0-rc.1-windows-x64.exe` | NSIS 安装程序。候选没有 Authenticode 签名；系统信任及组织策略须按实际下载环境验收。 |
| Ubuntu 22.04 / X11，amd64 | `hop-input_2.0.0~rc1_amd64.deb` | 使用 apt 安装本地包以解析依赖；纯 Wayland 会话暂未支持。 |
| HarmonyOS PC/二合一，API 20+，arm64 | `Hop-2.0.0-rc.1-harmonyos-unsigned.hap` | 用户自行签名并侧载。键鼠注入和后台运行需系统授权；剪贴板读取还取决于签名 profile 与设备授权。应用商店分发不在范围内。 |

macOS/HarmonyOS 包内版本显示 2.0.0。候选批次使用 2.0.0-rc.1；Debian 使用 `2.0.0~rc1`，使其在 apt 排序中早于 2.0.0。旧的双平台 v1 文件不属于本批候选。

- [macOS 安装及授权](../INSTALL_MAC.md)
- [HarmonyOS 签名与侧载](../INSTALL_HARMONYOS.md)
- [Windows 构建、安装和验证](../../apps/windows/installer/README.md)
- [Linux 安装、依赖和验证](../../apps/linux/README.md)

## 暂停后复验及新增证据

- 共享核心：CTest 4/4。通过共享 wire、焦点、快捷键、C ABI 和 golden vectors。TLS 回环和假 SDK 另测。
- macOS：真实 Xcode Release 通用构建通过，550 项离屏 AppKit 检查。独立回环验证双指纹配对、发起方 HPSL、固定证书 TLS、重复屏幕 ID 隔离、闲置设备掉线不干扰活动焦点、输入/返回/重连。剪贴板使用内存后端验证分片、初始快照、冲突时保留本地改动、退出后的写入撤销及超时。Quartz 注入以假后端验证。二维布局校验覆盖部分边缘重叠、方向、多屏及位置持久化；本机没有可用的 Developer ID Application 证书。
- HarmonyOS：DevEco 构建与原生 TCP 回环通过。用假输入 SDK 验证两块显示器、负坐标、目标屏切换、剪贴板版本基线、分片与双向消息、释放键鼠先于剪贴板任务结束、后续焦点拒绝旧写入。真实扩展屏、ArkTS 图像处理和系统剪贴板授权仍待设备验证。
- Windows：最终自动布局候选通过原生 CTest 27/27；实际 Windows 工作区运行时与模拟捕获在本机验证本机→A→B，且下一台必须等待上一台 RELEASE_ACK 和旧剪贴板任务排空。与 Mac 主机/接收端、HarmonyOS 接收线程在隔离回环通过安全配对、输入、滚轮和释放。用独立应用标识和最终载荷验证 NSIS 安装、升级及卸载；真实 Windows 托盘、物理钩子和局域网仍待统一验收。构建机没有符合条件的 Authenticode 证书。
- Linux：WSL 171 项和私有 Xvfb 18 个场景通过，覆盖实际 X11 捕获与注入、GTK 主从界面、双向文本/PNG、复制冲突和撤销。另用 XTest 在私有 X11 显示注入移动，生产捕获子进程驱动 A→B→A 自动穿越。与原生 Windows 双角色以及 HarmonyOS 假 SDK 接收端通过适配器回环。Ubuntu 22.04.5 amd64 上模拟 apt 求解及隔离安装、启动、重装和卸载通过。WSL 不能代表真实 LAN、多显示器 GPU/缩放或 Wayland 验收。

## 统一验收顺序

当前 v2 使用中立状态入场：先松开键鼠再穿越。远端使用时可发送多键组合、Caps Lock 和鼠标按钮；跨设备移交已按下的按键/按钮，以及跨越时自动迁移 Caps Lock 锁存状态，仍是后续功能。

1. 核对四份候选包的 SHA-256、版本及架构。分别在干净目标机安装、启动、授权和卸载一次。鸿蒙记录自行签名步骤、允许的 profile 以及侧载结果。Mac 不将 ad-hoc 测试包描述为正式公证下载包。
2. 同一局域网内逐一配对。Mac、Windows、Linux 分别充当主机；覆盖其余系统作为接收端。核对双端完整指纹；错误指纹、未经同意的连接及被解除配对的证书必须失败。验证主动断开、重新连接、角色切换和退出后恢复。
3. 三台以上电脑排列在二维空间，含一台多屏电脑。验证上、下、左、右、部分重叠边缘、相同屏幕编号隔离、远端之间穿越、离线设备和重连后的保存位置；记录单目标或不支持的桌面会话。
4. 在每台目标机验证多键组合、Caps Lock、Command/Windows 键、复制粘贴习惯、侧键、双轴滚轮。检查鸿蒙外接滚轮 1.5 倍基线。分别修改两台目标机的指针灵敏度、滚轮速度和方向。以真实鼠标实测 60/120/240/500/1000 Hz 档位的流畅度、CPU 和释放延迟。
5. 用纯文本、PNG、文件列表和大图验证双向剪贴板：仅同步当前焦点、文件列表不传、当地修改优先。切设备、撤权、休眠、网络断开和应用退出时，验证不会写到旧设备，按键、鼠标和光标可恢复。
6. 合并用户统一验收反馈后再发布 GitHub Release，并在 [README](../../README.md) 更新下载和签名信息，附真实支持矩阵、安装说明及校验和。将功能限制与尚未实测的环境保留在发布说明中。

本地复验目录为 `artifacts/test-build/resume-20261110/`，候选包和 `SHA256SUMS` 位于其 `rc-bundle/` 子目录。将最终包放在同一目录后可执行 `shasum -a 256 -c SHA256SUMS`。候选阶段的结果和正式发行包应分开标识，源码变化后重新打包并更新哈希。
