# HarmonyOS PC 构建、调试签名与安装 / Build, sign, install

## 支持条件与验证边界

- 目标是**鸿蒙电脑 / 2-in-1**，HarmonyOS6 / API20+；不支持手机/平板，不以无障碍或ACL替代系统注入授权。
- 工程采用Stage ArkTS + NAPI C++，HarmonyOS SDK需含HMS `@kit.DeskTopExtensionKit` 顶栏扩展。仅OpenHarmony SDK不足以构建此UI。
- 本次编译环境为DevEco26 / bundled API26 SDK；产品兼容下限仍为20。开发环境曾只读检测到API26的2-in-1，但没有用本工程做真实安装/注入，**不能声称HarmonyOS6/API20实测通过**。
- 已产出的 `entry-default-unsigned.hap` 是**未签名构建包**。Hvigor日志即使列出 `SignHap` 任务，只要警告 `No signingConfig found`，就不能叫已签名包。

## 1. DevEco打开与构建

在DevEco Studio选择Open，打开 `apps/harmonyos`。SDK选择HarmonyOS（包含桌面扩展）。包名保持 `dev.hop.input`，不得随意换成其他应用以借用签名。

当前构建工具版本的SDK字段兼容规则已按本机plugin检查：

```json
{
  "targetSdkVersion": "26.0.0",
  "compatibleSdkVersion": "6.0.0(20)",
  "runtimeOS": "HarmonyOS"
}
```

API26及以上的新格式不是 `26.0.0(26)`；旧版API20仍采用 `6.0.0(20)`。不要为绕过构建而将最低API改成26。其他DevEco版本按其自身schema核对。

本机可复现命令（从仓库根目录启动，子shell进入工程；不会更改全局SDK设置）：

```sh
(
  cd apps/harmonyos
  export DEVECO=/Applications/DevEco-Studio.app/Contents
  export NODE_HOME="$DEVECO/tools/node"
  export PATH="$NODE_HOME/bin:$DEVECO/tools/ohpm/bin:$PATH"
  export DEVECO_SDK_HOME="$DEVECO/sdk"
  export JAVA_HOME="$DEVECO/jbr/Contents/Home"
  "$NODE_HOME/bin/node" "$DEVECO/tools/hvigor/bin/hvigorw.js" --mode module \
    -p product=default -p module=entry@default -p buildMode=debug assembleHap \
    --no-daemon --no-parallel --stacktrace
)
```

使用DevEco bundled Node（本机24.14.1），不要随便换为旧Homebrew Node；工具所需Node版本已写在其README。没有registry产品依赖；NAPI类型为工程内本地类型。

默认输出：`apps/harmonyos/entry/build/default/outputs/default/entry-default-unsigned.hap`。本机用户的Hvigor全局配置把缓存放在工作区外，因此部分中间产物/日志可能在那里；不要把缓存路径问题误认成源码API不支持。

## 2. 无设备时的实际native代码离线测试

从仓库根目录：

```sh
python3 apps/harmonyos/tests/run_native_tests.py
```

测试编译生产 `protocol.h`、配对状态机、`injector.h` 与 `client.cpp`，但使用**测试专用假OH_Input函数**记录动作，不链接真实系统注入库。覆盖黄金帧、实际Client localhost TCP、配对确认/落盘屏障、epoch/心跳、按键按钮释放等。通过不等于设备授权/真实拖拽/系统常驻通过。

## 3. 鸿蒙电脑开启开发者模式与调试

系统菜单名称可能随HarmonyOS版本变化，以设备设置为准：

1. 在设置的“关于本机/设备”中按系统说明启用开发者选项，进入开发者选项打开USB调试或无线调试。
2. 只向你信任的这台Mac授予调试授权。无线调试使用设备展示的配对/连接流程，不能默认任意LAN设备已授权。
3. 在DevEco的Device Manager确认设备在线且类型为PC/2-in-1。
4. 使用SDK附带hdc确认目标，避免把HAP装到另一台设备：

```sh
HDC=/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc
"$HDC" list targets
"$HDC" -t YOUR_DEVICE_TARGET shell param get const.product.model
"$HDC" -t YOUR_DEVICE_TARGET shell param get const.product.devicetype
"$HDC" -t YOUR_DEVICE_TARGET shell param get const.ohos.apiversion
"$HDC" -t YOUR_DEVICE_TARGET shell bm get -u
```

UDID属于设备标识，只用于签发你自己的调试profile，不要公开提交。`bm get -u`输出格式依系统版本而定；若不支持，使用DevEco设备信息提供的UDID。

## 4. 申请/配置自己的调试签名（不可跳过）

建议在DevEco使用合法的自动调试签名流程：

1. 使用自己的华为开发者账号登录DevEco（这是**开发工具签名身份**，不是给Hop添加产品账号体系）。
2. Project Structure → Signing Configs / signingConfigs，选择default产品，启用自动签名并选择已授权的目标设备；对应应用标识必须为本工程的 `dev.hop.input`。
3. 若需要手动签名，在开发者控制台登记应用和设备UDID，签发调试证书与该应用/设备的调试profile；在工程Signing Configs中填写你的keystore、证书、profile及alias。名称依DevEco版本变化。
4. 重新Build Hap(s)/assembleHap，检查日志不再有 `No signingConfig found`，输出出现实际 `entry-default-signed.hap`（以IDE输出为准）。用IDE签名验证工具验证签名、包名、设备UDID和有效期。
5. 私钥、密码、p12、p7b、keystore不要提交git或分享。不要复制其他项目的profile，也不要自造一个不能通过设备信任校验的“签名”来冒充可安装包。

**当前工程没有你的有效调试证书/profile，没有产出可安装的签名HAP。** 这项需要开发者身份与设备授权，不能由未签名构建成功推导。证书/profile有效期以实际签发内容为准；到期后用原固定包名重新签发并重签，不承诺固定14天/180天。

## 5. 安装与启动（签名完成后）

核对签名和来源后，将下列占位路径替换为你实际的签名包；不要把unsigned文件改名成signed：

```sh
HDC=/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc
"$HDC" -t YOUR_DEVICE_TARGET install -r /ABSOLUTE/PATH/entry-default-signed.hap
"$HDC" -t YOUR_DEVICE_TARGET shell aa start -a EntryAbility -b dev.hop.input
```

或使用DevEco Run安装启动。安装失败先检查系统/API、包名、profile中UDID、调试授权和证书有效期，**不要关闭签名校验、不要申请CONTROL_DEVICE或改成无障碍旁路**。

首次启动不应显示常驻主窗口，只应出现顶部状态栏Hop图标。点图标打开小面板：

1. 允许系统通知，确认合法TASK_KEEPING长时任务与常驻通知成功；保活失败时连接禁用并显示原因，不伪装成音频/下载。
2. 点Authorize请求系统输入注入许可；只有系统回调授权且查询结果AUTHORIZED才连接/注入。被拒绝时不要无限重复弹窗。
3. 列表选择Mac，或在开发手填IP入口输入可信LAN地址；Mac先打开配对窗口。
4. 核对同一六位码，两边确认；token落盘Preferences后才确认提交。关闭面板仍应保持连接和常驻通知。
5. 主动Disconnect应停止自动重连。Unpair清理本地绑定，在线时也通知对端；离线另一端需要手动清理。

## 6. 真机验收 / 故障记录

照 `docs/ACCEPTANCE.md` 逐项实测并记机型/系统/API/签名。特别验证系统弹窗的显示/撤销时机、全局像素坐标、按住拖拽/多按钮、CapsLock、返回边缘、后台闲置和通知。头文件没有pressed-buttons数组setter，不可仅凭MOVE调用正确就宣称拖拽通过。

不要将API26测试结果扩大为API20最低版本通过。M1–M4的真实出口在完成清单前保持未通过。

## English summary

Use the HarmonyOS SDK including HMS desktop extensions, minimumAPI20. The current unsigned HAP proves compilation/packaging only. Enable developer mode and authorize debugging on your own PC/2-in-1. Sign with **your own valid debug certificate and an app/device-specific profile for `dev.hop.input` and the target UDID**, ideally using DevEco's automatic signing. Verify credentials/expiry and install the actual signed output with `hdc -t TARGET install -r SIGNED_HAP`, then start EntryAbility. Never rename unsigned output, borrow another app's profile or bypass signature/input authorization. Consent to notifications and system injection from the statusbar workflow; confirm the matching pairing code on both devices. Native fake-SDK tests are not real injection/tray/background acceptance. Follow the separate unchecked two-device checklist before daily use.
