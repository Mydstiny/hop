# Mac 构建、签名与安装

要求：macOS14+；Apple Silicon 或 Intel。开发依赖仅 Apple Command Line Tools（CLT原生构建）或完整 Xcode（工程构建）、Python3（生成共享映射/离线测试）。没有第三方运行库。

## 1. 原生 CLT 构建（当前环境可用）

仓库根目录：

```sh
python3 tools/generate_keymap.py --check
bash apps/macos/scripts/verify.sh
bash apps/macos/scripts/package.sh
```

默认同时编译 arm64 与 x86_64，最低系统14.0。产物：

- `apps/macos/build/Hop.app`
- `apps/macos/build/Hop-macos-universal-adhoc.zip`
- `apps/macos/build/critique/build-test.json` / `build-test.log`

这是同一组生产 Swift/AppKit 源码，不是Web壳或假客户端。脚本不会启动应用、授予权限或捕获你的输入。CLT编译通过不等于真实Xcode工程构建、Intel运行或macOS14真机通过。

## 2. 完整 Xcode 工程

打开 `apps/macos/Hop.xcodeproj`，选择 Hop scheme，目标My Mac。保持Deployment Target14.0、`LSUIElement=true`与固定Bundle ID，构建Release。命令行：

```sh
bash apps/macos/scripts/xcode-build.sh
```

脚本不更改全局 `xcode-select`。它现在自动识别已选中的完整Xcode，以及系统/用户Applications目录中已安装的Xcode（含改名或空格路径，优先正式版）；只给当前构建进程设置 `DEVELOPER_DIR`。因此装好Xcode后，即使全局仍指向CLT，也能直接重跑上述命令。显式提供的 `DEVELOPER_DIR` 优先，错误覆盖不会被静默忽略。只读设备预检使用同一发现逻辑。

最初检查时未找到完整Xcode；安装并选中Xcode27.2后曾出现首次许可/组件检查exit69。已请求打开Xcode处理首次设置，没有代替用户同意许可；随后该检查exit0，真实Release工程构建exit0 / BUILD SUCCEEDED，产物复核双架构/最低14.0/LSUIElement=true，见[实际工程报告](<../critique/xcode-build.json>)。发现工具的11项文件系统/模拟测试和 `xcodebuild -version` 成功本身仍不代表工程构建。可单独查看：

```sh
python3 tools/resolve_xcode.py
```

### 没有安装Xcode时

1. 从 [Mac App Store官方Xcode入口](<https://apps.apple.com/app/xcode/id497799835>) 获取Xcode，或从Apple自己的开发者下载渠道获取。不要下载第三方“精简/破解Xcode”，不要把CLT目录伪装成完整IDE。
2. 如系统要求，在App Store自行登录Apple账户；打开安装完成的Xcode，亲自处理首次许可和必要组件设置。本工程只需macOS平台，暂不必安装iOS/watchOS/tvOS/visionOS模拟器。下载与展开需额外临时空间，以系统实际要求为准。
3. Xcode本身免费；此工程构建使用 `CODE_SIGNING_ALLOWED=NO`，不要求购买Apple开发者会员。后续Developer ID发布签名/公证是独立事项。
4. 完成后运行上述构建命令。非标准安装位置可用 `DEVELOPER_DIR=/ABSOLUTE/PATH/Xcode.app/Contents/Developer bash apps/macos/scripts/xcode-build.sh`；也接受显式Xcode.app路径。不需要执行全局 `sudo xcode-select --switch`。

Mac原生自用包仍可通过CLT路线构建，不必为使用已有ad-hoc包等待完整Xcode；但CLT通过不替代严格要求的真实工程构建。若安装Xcode后全局切换导致首次许可阻止原生脚本，可只为命令指定 `DEVELOPER_DIR=/Library/Developer/CommandLineTools`；根统一离线验证现在默认这样使用已安装CLT，保留显式覆盖且不改全局设置。完整工程脚本和只读预检另外检查 `-checkFirstLaunchStatus`，不会把版本命令exit0误认为首次设置完成。

只有CLT时可额外运行以下**静态**检查；它不等价于Xcode构建：

```sh
python3 tools/check_xcode_project.py
python3 tools/test_xcode_project.py
```

检查用Apple原生plutil解析工程，验证当前受支持的显式对象图、全部Swift源文件、资源生成/复制、Info.plist、最低系统/架构意图和共享scheme；17项正/反测试覆盖遗漏InputOwnership、悬空引用、错误资源、架构/最低版本漂移等。它不是完整pbxproj或build-settings求值器，不展开 `ARCHS_STANDARD`，也不验证签名/GUI运行。Debug明确是active-arch-only；Release helper显式请求两种架构。共享scheme没有TestAction，离线原生测试由单独脚本运行。

## 3. 自用 ad-hoc 版安装

1. 解压通用构建包，将 `Hop.app` 拷到你自己的Applications目录（例如 `~/Applications`）。不要直接在构建缓存路径日常运行，路径与签名变化可能影响TCC授权。
2. 核对来源和SHA-256。当前包为 **ad-hoc签名、未公证**，不是Developer ID发布包。Gatekeeper可能阻止双击。
3. 在系统「隐私与安全性」中针对你确认来源的Hop使用“仍要打开”，或按系统提供的右键打开流程操作；不关闭全局Gatekeeper。
4. 只有确认是你自己构建且信任的包时，才可对**单个准确路径**移除隔离属性，不可对系统/下载目录递归操作：

```sh
xattr -dr com.apple.quarantine "$HOME/Applications/Hop.app"
open "$HOME/Applications/Hop.app"
```

应用只应出现在菜单栏，不应打开主窗口。默认共享关闭。

## 4. 辅助功能 / 输入监控 / 局域网

- 点菜单栏Hop，在Popover中按引导打开系统设置→隐私与安全性→辅助功能；允许准确的Hop应用。
- 若系统要求输入监控也应授权该应用，按提示退出重开。没有权限时不可启用捕获、更不能隐藏/冻结光标。
- 首次监听LAN可能出现防火墙/本地网络许可；允许同一可信局域网内发现与TCP24890连接，不要映射公网端口。
- 在Popover选择鸿蒙相对Mac的左/右方向，打开配对窗口；鸿蒙选中Mac后核对六位码，**两端都确认**。之后凭据存Keychain。
- 远程输入时使用 **Ctrl+Option+Command+Esc** 急停。首次真机试验应备一个直接连接Mac的备用输入设备。

应用不承诺登录前、锁屏安全界面、系统保留媒体键、任意多屏拓扑。崩溃/SIGKILL后的系统光标清理必须按验收清单实测，不能因为存在恢复函数就认为已证明。

## 5. 可选 Developer ID 签名与公证（需要你自己的Apple开发者身份）

本次机器 `security find-identity -v -p codesigning` 返回0个有效身份；未完成Developer ID签名/公证，不产生假签名身份。以下是未来有证书时的操作，不会自动执行：

```sh
codesign --force --options runtime --timestamp \
  --sign "Developer ID Application: YOUR NAME (TEAMID)" \
  apps/macos/build/Hop.app
codesign --verify --strict --verbose=2 apps/macos/build/Hop.app

ditto -c -k --keepParent apps/macos/build/Hop.app apps/macos/build/Hop-notarize.zip
xcrun notarytool submit apps/macos/build/Hop-notarize.zip \
  --keychain-profile YOUR_NOTARY_PROFILE --wait
xcrun stapler staple apps/macos/build/Hop.app
spctl --assess --type execute --verbose=2 apps/macos/build/Hop.app
```

Notary凭据应由开发者自行保存在Keychain，不能写进仓库。公证后重新打包，确保包内是staple过的app。Team ID、Bundle ID保持稳定，否则辅助功能授权与Keychain访问可能重新确认。无需添加云账号到产品。

## English

Build with `bash apps/macos/scripts/verify.sh` then `package.sh`; the universal native app targets macOS14+, arm64 and Intel. The CLT route is validated separately from the real Xcode-project route. The current package is **ad-hoc signed, not notarized**. Install only a trusted self-built copy, authorize Accessibility/Input Monitoring and LAN as prompted, then pair from the menu-bar popover by confirming the same six-digit code on BOTH devices. Sharing starts disabled. Emergency: **Control+Option+Command+Escape**. Keep fallback local input during initial acceptance. Developer ID/notarization requires your own certificate and must not be claimed until actually verified.
