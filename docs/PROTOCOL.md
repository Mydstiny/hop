# Hop Protocol v1 — 唯一线协议规格

状态：v1 frozen，2026-11-08。两端与 mock 必须以本文为准；与 Deskflow/Synergy 不兼容。仅 1 Mac server + 1 HarmonyOS PC client；同一时刻最多一个已认证 TCP 会话。

## 1. 传输与帧

使用原生 TCP，Mac 监听默认 24890（可在开发工具覆盖），mDNS `_hop._tcp`，TXT `v=1`、`id=<Mac UUID>`。不包含 token/配对码。无 TLS，仅限可信局域网；明文 token 和键鼠事件可能被监听或篡改，六位码不能防御主动中间人。不得暴露公网。

每帧：`uint32_be(128)` + **128 字节**消息体。全部整数大端，signed 使用二补码；禁止直接发送带 padding 的 C struct。最大帧长也为128；长度不等立即关闭，不分配该长度内存。接收必须处理逐字节分片、粘包、EOF 和短写。未知版本/类型、非零保留字节、非法字段、状态越权直接关闭并首先恢复/释放输入。

统一消息头（8B）：

| offset | size | 字段 |
|---|---:|---|
|0|1|version = 1|
|1|1|type = 1..10|
|2|1|flags：仅 HELLO 可为1（HELLO_ACK）；其余为0|
|3|1|reserved = 0|
|4|4|epoch：HELLO/KEEPALIVE/BYE 为0；其余为当前非零穿越代号|
|8|120|payload，下表未用区域必须补零|

恰好10个顶层类型。HELLO_ACK 是 HELLO 的 flags=1；KEY_DOWN/KEY_UP 是 KEY 的 action。这解决需求“10种”与枚举12个名称的计数冲突，不删去任何语义。只有 HELLO/ACK 可承载配对阶段，不添加配对消息类型。

## 2. 消息布局（下列 offset 相对 payload）

### 1 — HELLO / HELLO_ACK (120B)

| offset | size | 字段 |
|---|---:|---|
|0|1|phase: 0 RESUME, 1 PAIR, 2 CHALLENGE, 3 CONFIRM, 4 READY, 5 COMMIT, 6 REJECT|
|1|1|role: 1 Mac, 2 HarmonyOS|
|2|1|direction: 0 HarmonyOS在Mac左侧，1右侧；client首次请求可填1，之后服从Mac|
|3|1|reserved0|
|4|4|code: 0或100000..999999；CHALLENGE/CONFIRM/COMMIT 必须带同一码|
|8|4|屏幕宽度（物理像素），1..65535|
|12|4|屏幕高度（物理像素），1..65535|
|16|16|发送者持久UUID原始16字节（不是ASCII），禁止全零|
|32|16|本会话nonce，初次由client安全随机生成，所有回复原样回显|
|48|32|随机token，仅 RESUME / COMMIT / READY 使用，其余全零|
|80|32|设备名 UTF-8，最多31字节，NUL结束并补零，不得截断UTF-8字符|
|112|8|reserved0|

发起连接必须在5秒内发client HELLO。配对/授权会话上限60秒，配对中也发心跳；未认证不得处理任何输入。六位码和token使用系统安全随机源（Mac SecRandomCopyBytes；Harmony /dev/urandom；Python secrets）。一次连接的nonce不复用，拒绝不同nonce。

**新配对（可重复确认不重复提交）**：

1. Client 仅在用户选择Mac后发送 HELLO PAIR，token=0，nonce随机。Mac若已有绑定则拒绝陌生设备，需先解绑。Mac生成六位码和暂存随机32字节token，回复 ACK CHALLENGE（token=0）。两端仅在小面板显示该码。
2. Client用户确认后发送 HELLO CONFIRM（同code、nonce，token=0）。Mac本地确认单独记状态。任一方未确认，禁止COMMIT与持久化。拒绝/超时/断开清除所有pending。
3. Mac两项确认都收到后，发送 ACK COMMIT（同code、nonce，token=random）。Client验证code/nonce并仅在自身确认过后持久化 Mac UUID + token + 地址；回复 HELLO COMMIT（同token/code/nonce）。
4. Mac收到匹配的提交确认后持久化 client UUID + token（Keychain），再回复 ACK READY（token同上）。此时连接认证成功，输入默认停留Mac。COMMIT丢失导致的半配对：Mac可以在同进程内保留已双确认的候选token60秒，以接受client携token RESUME，持久化后再READY；若Mac重启/窗口过期，客户端提示重新配对并清理孤立token，而不是永久重试。不得在收到client提交确认前将pending称为正式绑定。

**重连**：Client HELLO RESUME，带已存token与新nonce。Mac恒定时间比较绑定client UUID/token；成功 ACK READY，回显nonce与token并附自己的UUID/方向/尺寸。Client必须核对绑定Mac UUID/token/nonce再认证。失败 ACK REJECT，随后BYE/断开，不自动降级成无确认配对。

**解绑**：先本地恢复/远端释放，再BYE reason=4并清除本地绑定。已连接peer收到reason=4也清除绑定；不在线时另一端凭失效token重连会被拒绝，须在面板手动重新配对。不得允许离线未授权请求清除绑定。

### 2 — ENTER

Mac→client：payload `x:u16, y:u16, modifiers:u16, buttons:u16`（8B）。epoch为新非零递增代号，wrap跳过0。坐标绝对归一化[0,65535]。client授权有效且认证后才接受；初始化位置、注入修饰键/已按鼠标按键、进入REMOTE。

client→Mac 同类型/同payload/epoch 是注入成功确认（角色区分方向）；必须在300ms内确认。Mac**收到确认后**才隐藏光标、解除物理光标关联并开始吞输入，以防网络/授权失败丢失本地输入。ENTER等待期间本地输入不吞；离开边缘/按键状态变化可取消等待。客户端注入失败立刻BYE reason=3/断开释放。双方等待超时释放；Mac不因发出ENTER就隐藏光标。

x/y映射：`pixel=round(normalized*(dimension-1)/65535)`；反向 `normalized=round(pixel*65535/(dimension-1))`（dimension=1时为0）。方向右：client从左边缘内侧进入（至少2px）；方向左：从右侧内侧进入。y按各自屏幕高度比例对应。不实现屏幕扩展/任意多屏拓扑，使用各自默认主显示屏。

### 3 — LEAVE

Mac→client，payload `x:u16,y:u16,reason:u8`（5B）。reason 0正常返回，1急停/禁用/睡眠，2异常恢复。匹配epoch时释放全部注入按键与按钮、清除队列、回到LOCAL。Mac无需等回复，先恢复本地光标/输入；恢复到约定边缘内侧，防止立刻再次穿越。

### 4 — REQUEST_LEAVE

client→Mac，payload `x:u16,y:u16`（4B）。client只根据**自身成功注入的位置**检测返回边缘，禁止全局输入监听。方向右时client左缘返回；方向左时右缘返回。ENTER后必须先向屏幕内部移动至少4px才armed，避免入边缘即退出。第一次命中后暂停注入并释放所有注入按键/按钮，发送一次REQUEST_LEAVE，等待LEAVE；超时300ms主动断开，Mac同时恢复。

### 5 — MOUSE_MOVE

Mac→client，payload `x:u16,y:u16`（4B）。REMOTE时发送。绝对位置由Mac捕获delta累加到远端虚拟光标、约束到边界再归一化。不依赖Mac被固定的光标绝对值。移动最多120Hz，队列只保留最新一条；按钮/滚轮/键盘消息前flush最新移动，离开清空pending，不能丢弃或合并按键消息。

### 6 — MOUSE_BUTTON

Mac→client，payload `button:u8, action:u8, x:u16,y:u16,buttons:u16`（8B）。button 1左、2右、3中、4后退、5前进；action 0up/1down。buttons位0..4对应各button，包含本次更新后状态；高位0。坐标须先应用后注入按钮，拖拽时MOVE包含当前保持按下状态（本地维护，不增线字段）。跨机文件/剪贴板拖拽不支持；同一远端内部拖拽支持。

### 7 — WHEEL

Mac→client，payload `dx:i32,dy:i32`（8B），单位1/120逻辑步；正x向右，正y向下。接收注入为axis begin/update/end完整序列，无持续轴状态；平台适配可折算为浮点步数。发送端需统一系统自然滚动方向，不承诺触控板惯性/手势保真。每轴范围[-120000,120000]。

### 8 — KEY (KEY_DOWN / KEY_UP)

Mac→client，payload `mac_keycode:u16, modifiers:u16, action:u8, repeat:u8`（6B）。action 0up/1down；repeat 0/1，up必须repeat=0。物理键位而非字符/Unicode；映射源唯一为 `protocol/keymap.json`。modifier bits：0左Shift、1右Shift、2左Ctrl、3右Ctrl、4左Alt/Opt、5右Alt/Opt、6左Meta/Cmd、7右Meta/Cmd、8CapsLock；其余为0。修饰键本身也发KEY，flagsChanged转成down/up；CapsLock适配锁定状态，不用持续按下模拟。ENTER状态与每次KEY的修饰位需核对/修复，断线/离开必须释放实际按下集合。不发文本，不修改输入法。Fn/亮度等不可捕获或系统保留键在KEYMAP文档逐项标不支持，不猜映射。

### 9 — KEEPALIVE

双向，payload `monotonic_ms:u64`（8B），epoch0。每1秒发一次；任何收到且合法帧都更新本地单调lastReceive；3秒无合法帧就断开并恢复。无需ping回复（双方均发送）。上限10Hz，防止忙循环。

### 10 — BYE

双向，payload `reason:u8`（1B），epoch0。0用户断开/退出，1协议错误，2心跳/网络，3授权或注入失败，4解绑。收到后首先恢复/释放输入，随后关socket；BYE本身是best effort，不能阻塞恢复。reason=4仅已认证会话可触发对端清除绑定。

## 3. 状态与安全恢复不变量

Mac：DISCONNECTED → HANDSHAKE → LOCAL → ENTER_PENDING → REMOTE → LOCAL。任何状态都能恢复到LOCAL/DISCONNECTED。断连、协议错误、3秒超时、CGEventTap被禁用、失去辅助功能、显示器变化、睡眠、用户禁用、退出、Ctrl+Opt+Cmd+Esc触发**同一个幂等的同步恢复入口**：先取消吞输入/清pending，然后关联鼠标、显示光标、warp边缘内侧；最后才发LEAVE/BYE/改UI。急停事件本身不发送给远端。捕获回调异常优先透传，不得卡网络。无授权时绝不隐藏/冻结。

client：DISCONNECTED → AUTHORIZING/HANDSHAKE → LOCAL → REMOTE → LEAVING → LOCAL。授权撤销/注入错误/断连/离开释放实际注入的pressed集合；拒绝旧epoch和非REMOTE输入，不得根据任意旧MOVE注入。注入失败时立即断连，不用旁路。只能单线程有序处理输入，TCP读写在非UI线程；回调通过NAPI threadsafe function安全送UI。禁止在未授权前尝试注入。接收循环在每次 socket 读取批次之前检查系统授权，一个批次最多读取 4096 字节；该检查不逐帧重复，系统仍对实际注入执行权限控制，撤权与读取批次之间的竞争及设备效果需真机验证。

Mac远程期间仅全局键鼠物理事件被转发并吞掉，系统关键异常必须恢复；未支持键可以选择不转发但不得让本地组合键误动作（急停除外）。退出远端前先释放远端，再按平台安全方式让本地重新获得后续输入，不能凭空给本地应用制造点击/键盘动作。断电/进程崩溃无法运行finally，需验证系统对CGEventTap/光标关联的进程清理行为，列为真机验收项目，不以源码证明替代。

物理按下归属是独立于线协议pressed集合的本地安全账本，不改变任何帧：远程期间吞掉down的键/按钮（含不支持键）在返回后仍需隔离其repeat/up/drag尾部，直到真实物理松开；新的本地输入正常可用，不能通过合成本地down“补齐”。进入前已经透传到本地的修饰键/按钮保留本地归属，它们在远程期间的真实release需要在转发远端的同时也透传本地，避免原本地down永远没有up。禁用可保留仅清理归属的LOCAL drain tap，不得继续冻结或把新输入发远端。退出、权限撤销、tap不可用/设备断开时不能保证继续隔离尾部，必须列为真机边界。急停先同步恢复，然后将网络/teardown/UI延后到回调返回后的受代号保护任务，避免在活动tap回调中移除自身或进行AppKit布局。

## 4. 一致性测试

mock-peer使用Python标准库。至少验证：10类型/12逻辑动作往返、固定长度黄金字节、分片/粘包、非法长度/版本/flags/保留字节、越权输入、nonce/code/token错误、双确认前无token、重连拒绝降级、epoch过期、3秒超时、移动合并及事件前flush、离开释放。两端须与相同黄金帧交叉验证；禁止将mock/socket测试报告为真机注入通过。
