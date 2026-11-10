import { ui, stateLabel, messageLabel } from '../HopI18n';
import { common } from '@kit.AbilityKit';
import { statusBarManager } from '@kit.DeskTopExtensionKit';
import { HopService, HopViewState, MacPeer } from '../HopService';
import { HopV2Coordinator, HopV2ViewState } from '../HopV2Coordinator';
import { V2NearbyPeer } from '../HopV2Discovery';
import { V2TrustedPeer } from '../HopV2Vault';

@Entry
@Component
struct Index {
  @State view: HopViewState = HopService.instance.view;
  @State address: string = '';
  private service: HopService = HopService.instance;
  private secure: HopV2Coordinator = HopV2Coordinator.instance;
  @State secureView: HopV2ViewState = HopV2Coordinator.instance.view;
  @State showSecure: boolean = true;
  @State secureAddress: string = '';
  @State securePort: string = '24891';
  @State showOptions: boolean = false;
  @State showDevices: boolean = false;
  @State showManual: boolean = false;
  @State manualPeer: V2TrustedPeer | undefined = undefined;
  @State manualError: string = '';
  private readonly clipboardAvailable: boolean = true;
  private readonly ink: string = '#252438';
  private readonly subtle: string = '#5C5A71';
  private readonly accent: string = '#5140A2';
  private readonly tint: string = '#EBE8F7';
  private readonly buttonFill: string = '#EAE9EF';
  private readonly line: string = '#E8E4F0';
  private readonly pageScroll: Scroller = new Scroller();
  private heightTimer: number = -1;
  private panelHeight: number = 240;
  private resizePanel(height: number): void {
    if (!Number.isFinite(height) || height < 1) { return; }
    const requested = Math.max(170, Math.min(450, Math.ceil(height + 2)));
    if (this.heightTimer >= 0) { clearTimeout(this.heightTimer); this.heightTimer = -1; }
    if (Math.abs(this.panelHeight - requested) <= 4) { return; }
    this.heightTimer = setTimeout(() => {
      this.heightTimer = -1;
      const context = this.service.getContext();
      try {
        if (context) {
          statusBarManager.updateQuickOperationHeight(context, requested);
          this.panelHeight = requested;
        }
      } catch (_) {}
    }, 180);
  }
  aboutToDisappear(): void {
    if (this.heightTimer >= 0) { clearTimeout(this.heightTimer); this.heightTimer = -1; }
  }
  private toggleSettings(): void {
    this.showOptions = !this.showOptions;
    this.pageScroll.scrollTo({ xOffset: 0, yOffset: 0, animation: false });
  }
  private fingerprint(value: string): string {
    let result = '';
    for (let i = 0; i < value.length; i += 8) { result += (i === 32 ? '\n' : i > 0 ? ' ' : '') + value.substring(i, i + 8); }
    return result;
  }
  private port(): number { const value = Number(this.securePort); return Number.isInteger(value) ? value : 0; }
  aboutToAppear(): void {
    const context = this.service.getContext();
    if (this.showSecure && context) { this.secure.initialize(context); }
  }
  private async connectTrusted(peer: V2TrustedPeer): Promise<void> {
    this.service.disconnect();
    for (let i = 0; i < 40 && this.view.state !== 'disconnected'; i++) {
      await new Promise<void>((resolve: () => void) => { setTimeout(resolve, 50); });
    }
    await this.secure.connectTrusted(peer, this.secureAddress, this.port(), this.view.state === 'disconnected',
      this.view.authorization === 2, this.view.background);
  }
  private idle(): boolean {
    return this.secureView.state === 'disconnected' || this.secureView.state === 'error';
  }
  private connected(): boolean {
    return this.secureView.state === 'ready' || this.secureView.state === 'remote';
  }
  private comparing(): boolean {
    return this.secureView.state === 'candidate' || this.secureView.state === 'pairing';
  }
  private revealDevices(): void {
    this.showDevices = !this.showDevices;
    this.showManual = false;
    this.manualPeer = undefined;
    this.manualError = '';
  }

  private chooseTrusted(peer: V2TrustedPeer): void {
    this.manualPeer = peer;
    this.showManual = true; // Keep the pinned device selected if the endpoint fails.
    this.manualError = '';
    if (this.secure.canReconnect(peer)) {
      this.service.disconnect();
      this.secure.reconnect(peer, this.view.state === 'disconnected', this.view.authorization === 2, this.view.background);
      return;
    }
    const matches = this.secureView.peers.filter((nearby: V2NearbyPeer) => this.nearbyName(nearby) === peer.name);
    if (matches.length === 1) {
      this.secureAddress = matches[0].host; this.securePort = String(matches[0].port);
      this.connectTrusted(peer);
    }
  }
  private nearbyName(peer: V2NearbyPeer): string {
    // macOS appends a per-process suffix to its Bonjour instance name.
    return peer.name.replace(/-[0-9a-f]{10}$/, '');
  }
  private submitAddress(): void {
    const port = this.port();
    if (!this.secureAddress.trim() || port < 1 || port > 65535) {
      this.manualError = ui('Enter an address and a port from 1 to 65535.', '请输入局域网地址和 1–65535 之间的端口。');
      return;
    }
    this.manualError = '';
    if (this.manualPeer) { this.connectTrusted(this.manualPeer); }
    else { this.service.disconnect(); this.secure.offer(this.secureAddress, port); }
  }

  @Builder
  private PageHeader(title: string, back: () => void) {
    Row() {
      Text(title).fontSize(18).fontWeight(FontWeight.Bold).fontColor(this.ink).layoutWeight(1)
      Button(ui('Back', '返回')).height(30).backgroundColor(this.buttonFill).fontColor(this.ink).fontSize(13)
        .onClick(back)
    }.width('100%')
  }

  @Builder
  private FingerprintCard(label: string, value: string) {
    Column({ space: 4 }) {
      Text(label).fontSize(12).fontWeight(FontWeight.Medium).fontColor(this.subtle).width('100%').maxLines(2)
      Text(this.fingerprint(value)).fontSize(12).fontFamily('monospace').fontColor(this.ink).width('100%').lineHeight(19)
        .copyOption(CopyOptions.None)
    }.width('100%').alignItems(HorizontalAlign.Start).padding({ top: 10, bottom: 10, left: 12, right: 12 })
      .borderRadius(10).backgroundColor('#F5F3FA')
  }

  @Builder
  private CompareCard() {
    Column({ space: 10 }) {
      Text(ui('Confirm pairing', '确认配对')).fontSize(20).fontWeight(FontWeight.Bold).fontColor(this.ink).width('100%')
      Text(ui('Check both fingerprints on both computers.', '请在两台电脑上核对下面的两组指纹。'))
        .fontSize(13).fontColor(this.subtle).width('100%')
      this.FingerprintCard(ui('This computer', '这台电脑'), this.secureView.localFingerprint)
      this.FingerprintCard(this.secureView.peerName, this.secureView.peerFingerprint)
      if (this.view.authorization !== 2) {
        Button(ui('Allow keyboard and mouse', '允许键鼠控制')).type(ButtonType.Normal).borderRadius(10)
          .width('100%').height(40).backgroundColor(this.accent).fontColor(Color.White).onClick(() => this.service.authorize())
      } else if (!this.view.background) {
        Text(ui('Allow Hop to run in the background before confirming.', '请先允许 Hop 在后台运行，再确认配对。'))
          .fontSize(13).fontColor(this.ink).width('100%')
      } else {
        Button(ui('Both fingerprints match', '两组指纹均一致')).type(ButtonType.Normal).borderRadius(10)
          .width('100%').height(40).backgroundColor(this.accent).fontColor(Color.White)
          .onClick(() => {
            if (this.secureView.state === 'candidate') {
              this.secure.approveCandidate(this.view.state === 'disconnected', this.view.authorization === 2, this.view.background);
            } else { this.secure.confirmExisting(); }
          })
      }
      Button(ui('Cancel pairing', '取消配对')).height(32).backgroundColor(Color.Transparent).fontColor(this.subtle)
        .onClick(() => this.secure.disconnect())
    }.width('100%').alignItems(HorizontalAlign.Start)
      .transition(TransitionEffect.OPACITY.animation({ duration: 160, curve: Curve.EaseOut }))
  }

  @Builder
  private DevicePicker() {
    Column({ space: 8 }) {
      this.PageHeader(ui('Connect a computer', '连接电脑'), () => this.revealDevices())
      if (this.secureView.trusted.length > 0) {
        Text(ui('Paired', '已配对')).fontSize(12).fontWeight(FontWeight.Medium).fontColor(this.subtle).width('100%')
        ForEach(this.secureView.trusted, (peer: V2TrustedPeer) => {
          Button(`${peer.name}  ›`).type(ButtonType.Normal).borderRadius(9)
            .width('100%').height(38).backgroundColor(this.tint).fontColor(this.accent).fontSize(14)
            .onClick(() => this.chooseTrusted(peer))
        }, (peer: V2TrustedPeer) => peer.fingerprint)
      }
      Text(ui('Nearby', '附近设备')).fontSize(12).fontWeight(FontWeight.Medium).fontColor(this.subtle).width('100%')
      if (this.secureView.peers.length === 0) {
        Text(this.secureView.discoveryFailed ?
          ui('Nearby discovery is unavailable. You can enter an address below.', '附近发现暂不可用，可在下方输入地址。') :
          ui('On the sharing computer, open Hop and choose Add computer.', '在主控电脑上打开 Hop，选择「添加电脑」。'))
          .fontSize(13).fontColor(this.subtle).width('100%')
      }
      ForEach(this.secureView.peers, (peer: V2NearbyPeer) => {
        Button(`${this.nearbyName(peer)}  ›`).type(ButtonType.Normal).borderRadius(9)
          .width('100%').height(38).backgroundColor('#F5F3FA').fontColor(this.ink).fontSize(14)
          .onClick(() => { this.service.disconnect(); this.secure.offer(peer.host, peer.port); })
      }, (peer: V2NearbyPeer) => `${peer.name}|${peer.host}|${peer.port}`)
      if (this.idle() && this.secureView.message.length > 0 && this.secureView.state === 'error') {
        Text(this.secureView.message).fontSize(13).fontColor('#9B392C').width('100%')
      }
      Button(this.showManual ? ui('Hide address', '收起地址') : ui('Enter address…', '手动输入地址…'))
        .height(32).backgroundColor(Color.Transparent).fontColor(this.accent).fontSize(13)
        .onClick(() => { this.showManual = !this.showManual; this.manualPeer = undefined; this.manualError = ''; })
      if (this.showManual) {
        Text(this.manualPeer ? `${ui('Connect to', '连接到')} ${this.manualPeer.name}` : ui('Connection address', '连接地址'))
          .fontSize(13).fontColor(this.ink).width('100%')
        Row({ space: 8 }) {
          TextInput({ placeholder: ui('LAN IP address', '局域网 IP 地址'), text: this.secureAddress })
            .layoutWeight(1).height(38).fontSize(14).borderRadius(8).backgroundColor('#F5F3FA')
            .onChange((value: string) => { this.secureAddress = value; this.manualError = ''; })
          TextInput({ placeholder: ui('Port', '端口'), text: this.securePort })
            .width(76).height(38).fontSize(14).borderRadius(8).backgroundColor('#F5F3FA')
            .onChange((value: string) => { this.securePort = value; this.manualError = ''; })
        }.width('100%')
        if (this.manualError.length > 0) {
          Text(this.manualError).fontSize(12).fontColor('#9B392C').width('100%')
        }
        Button(ui('Connect', '连接')).type(ButtonType.Normal).borderRadius(9).width('100%').height(38)
          .backgroundColor(this.accent).fontColor(Color.White)
          .onClick(() => this.submitAddress())
      }
    }.width('100%').alignItems(HorizontalAlign.Start)
      .transition(TransitionEffect.OPACITY.animation({ duration: 160, curve: Curve.EaseOut }))
  }

  @Builder
  private SecureSettings() {
    Column({ space: 10 }) {
      this.PageHeader(ui('Settings', '设置'), () => this.toggleSettings())
      if (this.clipboardAvailable) {
        Column({ space: 8 }) {
          Row({ space: 10 }) {
            Column({ space: 4 }) {
              Text(ui('Clipboard sync', '同步剪贴板')).fontSize(15).fontWeight(FontWeight.Medium).fontColor(this.ink).width('100%')
              Text(ui('Text and images while controlling this computer.', '控制这台电脑时同步文本和图片。'))
                .fontSize(12).fontColor(this.subtle).width('100%')
            }.layoutWeight(1).alignItems(HorizontalAlign.Start)
            Toggle({ type: ToggleType.Switch, isOn: this.secureView.clipboardEnabled })
              .accessibilityText(ui('Clipboard sync', '同步剪贴板')).selectedColor(this.accent)
              .onChange((enabled: boolean) => this.secure.setClipboardEnabled(enabled))
          }.width('100%')
          if (this.secureView.clipboardEnabled && !this.secureView.clipboardAuthorized) {
            Button(ui('Allow pasteboard access', '授权读取剪贴板')).height(32).backgroundColor(this.tint)
              .fontColor(this.accent).fontSize(13)
              .onClick(() => {
                const context = this.service.getContext() as common.UIAbilityContext;
                if (context) { this.secure.authorizeClipboard(context); }
              })
          }
          if (this.secureView.clipboardMessage.length > 0) {
            Text(this.secureView.clipboardMessage).fontSize(12).fontColor(this.subtle).width('100%')
          }
        }.padding(12).borderRadius(11).backgroundColor(this.tint).width('100%').alignItems(HorizontalAlign.Start)
      }
      if (this.secureView.trusted.length > 0) {
        Text(ui('Paired computers', '已配对电脑')).fontSize(12).fontWeight(FontWeight.Medium).fontColor(this.subtle).width('100%')
      }
      ForEach(this.secureView.trusted, (peer: V2TrustedPeer) => {
        Row({ space: 8 }) {
          Text(peer.name).fontSize(14).fontColor(this.ink).layoutWeight(1).maxLines(2)
          Button(ui('Forget', '解除配对')).height(32).backgroundColor(Color.Transparent).fontColor(this.subtle).fontSize(12)
            .onClick(() => {
              AlertDialog.show({
                title: ui('Forget this computer?', '解除这台电脑的配对？'), message: peer.name,
                primaryButton: { value: ui('Cancel', '取消'), action: () => {} },
                secondaryButton: { value: ui('Forget', '解除配对'), action: () => { this.secure.revoke(peer); } }
              });
            })
        }.width('100%').padding({ left: 12 }).borderRadius(9).border({ width: 1, color: this.line })
      }, (peer: V2TrustedPeer) => peer.fingerprint)
      Button(ui('Legacy Mac connection…', '旧版 Mac 连接…')).height(32).backgroundColor(Color.Transparent)
        .fontColor(this.subtle).fontSize(12)
        .onClick(() => { this.secure.disconnect(); this.showSecure = false; })
    }.width('100%').alignItems(HorizontalAlign.Start)
      .transition(TransitionEffect.OPACITY.animation({ duration: 160, curve: Curve.EaseOut }))
  }

  @Builder
  private ConnectionCard() {
    Column({ space: 9 }) {
      Column({ space: 4 }) {
        Row() {
          Text(ui('Receiving input', '接收键鼠')).fontSize(12).fontWeight(FontWeight.Medium)
            .fontColor(this.subtle).layoutWeight(1)
          Button(ui('Settings', '设置')).height(28).backgroundColor('#E0DBF0').fontColor(this.ink).fontSize(12)
            .onClick(() => this.toggleSettings())
        }.width('100%')
        Text(this.connected() ? this.secureView.peerName : this.idle() ? ui('Connect to a computer', '连接主控电脑') :
          this.secureView.state === 'reconnecting' ? ui('Reconnecting…', '正在重连…') : ui('Connecting…', '正在连接…'))
          .fontSize(19).fontWeight(FontWeight.Bold).fontColor(this.ink).width('100%').maxLines(2)
        Text(this.connected() ?
          (this.secureView.state === 'remote' ? ui('Receiving keyboard and mouse', '正在接收键鼠') :
            ui('Connected · Cross a screen edge to use this computer', '已连接 · 从主控端的屏幕边缘穿越')) :
          this.secureView.message || ui('Use another computer’s keyboard and mouse here.', '使用另一台电脑的键盘和鼠标。'))
          .fontSize(13).fontColor(this.subtle).lineHeight(19).width('100%')
      }.padding({ left: 13, right: 13, top: 8, bottom: 12 }).borderRadius(13)
        .width('100%').alignItems(HorizontalAlign.Start).backgroundColor(this.tint)
      if (this.idle() && this.view.authorization !== 2) {
        Button(ui('Allow keyboard and mouse', '允许键鼠控制')).width('100%').height(36)
          .fontSize(14).backgroundColor(this.accent).fontColor(Color.White).onClick(() => this.service.authorize())
      } else if (this.idle() && !this.view.background) {
        Text(ui('Relaunch Hop to restore background access.', '后台运行未就绪，请重新打开 Hop。'))
          .fontSize(13).fontColor('#9B392C').width('100%')
      } else if (this.idle()) {
        Button(ui('Choose computer', '选择电脑')).width('100%').height(36)
          .fontSize(14).backgroundColor(this.accent).fontColor(Color.White).onClick(() => this.revealDevices())
      } else {
        Button(this.connected() ? ui('Disconnect', '断开连接') : ui('Cancel', '取消')).width('100%').height(36)
          .fontSize(14).backgroundColor(this.buttonFill).fontColor(this.ink)
          .onClick(() => { this.secure.disconnect(); this.showDevices = false; })
      }
    }.width('100%').alignItems(HorizontalAlign.Start)
      .transition(TransitionEffect.OPACITY.animation({ duration: 160, curve: Curve.EaseOut }))
  }

  @Builder
  private SecurePanel() {
    if (this.comparing()) {
      this.CompareCard()
    } else if (this.showOptions) {
      this.SecureSettings()
    } else if (this.idle() && this.showDevices && this.view.authorization === 2 && this.view.background) {
      this.DevicePicker()
    } else {
      this.ConnectionCard()
    }
  }

  build() {
    Scroll(this.pageScroll) {
      Column({ space: 10 }) {
        if (this.showSecure) {
          this.SecurePanel()
        } else {
          this.PageHeader(ui('Legacy Mac', '旧版 Mac'), () => {
            this.service.disconnect(); const context = this.service.getContext();
            if (context) { this.secure.initialize(context); }
            this.showOptions = false; this.showSecure = true;
          })
        Row() {
          Column({ space: 2 }) {
            Text(ui('Receiver', '接收端')).fontSize(11).fontColor('#8E8E93')
            Text(stateLabel(this.view.state)).fontSize(16).fontWeight(FontWeight.Medium).fontColor('#1C1C1E')
          }.alignItems(HorizontalAlign.Start)
          Blank()
          Text(this.view.authorization === 2 ? ui('Authorized', '已授权') : ui('Not authorized', '未授权'))
            .fontSize(12)
            .fontColor(this.view.authorization === 2 ? '#1F6B4A' : '#8E8E93')
            .padding({ left: 8, right: 8, top: 4, bottom: 4 })
            .backgroundColor(this.view.authorization === 2 ? '#E7F3EC' : '#F2F2F4')
            .borderRadius(10)
        }.width('100%').alignItems(VerticalAlign.Center)

        if (this.view.message.length > 0) {
          Text(messageLabel(this.view.message)).fontSize(13).fontColor('#3A3A3C').width('100%').lineHeight(18)
        }

        if (!this.view.background) {
          Text(ui('Background task / persistent notification not active. Connections are disabled. Relaunch Hop to retry.', '后台任务或持续通知未启用，暂无法连接。请重启 Hop。'))
            .fontSize(12).fontColor('#A34329').lineHeight(17).width('100%')
            .padding(10).backgroundColor('#F8EBE6').borderRadius(8)
        }

        Row() {
          Text(this.view.authorization === 2 ? ui('System injection authorized', '已允许系统输入控制') : ui('System injection permission required', '需要允许系统输入控制'))
            .fontSize(13).fontColor('#1C1C1E').layoutWeight(1)
          if (this.view.authorization !== 2) {
            Button(ui('Authorize', '授权'))
              .fontSize(13).fontWeight(FontWeight.Medium).fontColor(Color.White)
              .height(32).backgroundColor('#1F6B4A')
              .onClick(() => this.service.authorize())
          }
        }.width('100%').alignItems(VerticalAlign.Center)

        if (this.view.code > 0) {
          Column({ space: 8 }) {
            Text(ui('Compare this code with the Mac', '请与 Mac 上的配对码核对')).fontSize(12).fontColor('#8E8E93').width('100%')
            Text(this.view.code.toString())
              .fontSize(32).fontWeight(FontWeight.Bold).fontColor('#1C1C1E').letterSpacing(6)
              .width('100%').textAlign(TextAlign.Center)
            Text(ui('Both devices must confirm. No token is saved until the Mac commits.', '两台设备都需要确认，完成后才保存配对。'))
              .fontSize(11).fontColor('#8E8E93').lineHeight(15).width('100%')
            Button(ui('Codes match — confirm', '配对码一致，确认')).width('100%').height(36)
              .fontSize(14).fontWeight(FontWeight.Medium).fontColor(Color.White).backgroundColor('#1F6B4A')
              .onClick(() => this.service.confirm())
            Button(ui('Cancel', '取消')).width('100%').height(36).type(ButtonType.Normal)
              .fontSize(14).fontColor('#1C1C1E').backgroundColor('#EFEFF1')
              .onClick(() => this.service.disconnect())
          }.width('100%').padding(12).backgroundColor('#F4F5F7').borderRadius(12)
            .alignItems(HorizontalAlign.Start)
        }

        if (this.view.paired && this.view.code === 0) {
          Button(ui('Connect paired Mac', '连接已配对的 Mac')).width('100%').height(36)
            .fontSize(14).fontWeight(FontWeight.Medium).fontColor(Color.White).backgroundColor('#1F6B4A')
            .enabled(this.view.state === 'disconnected')
            .onClick(() => this.service.connectBinding())
          Text(ui('Automatically reconnects while enabled and authorized. Disconnect pauses reconnection.', '启用并授权后自动重连；主动断开会暂停重连。'))
            .fontSize(11).fontColor('#8E8E93').lineHeight(15).width('100%')
        } else if (this.view.state === 'disconnected' && this.view.code === 0) {
          Text(ui('NEARBY MACS', '附近的 Mac')).fontSize(11).fontColor('#8E8E93').width('100%')
          if (this.view.discovery.length > 0) {
            Text(messageLabel(this.view.discovery)).fontSize(12).fontColor('#3A3A3C').width('100%')
          }
          ForEach(this.view.peers, (peer: MacPeer) => {
            Button(`${peer.name} · ${peer.host}`).width('100%').height(34)
              .fontSize(13).fontColor('#1C1C1E').backgroundColor('#F4F5F7').type(ButtonType.Normal)
              .onClick(() => this.service.connectPeer(peer))
          }, (peer: MacPeer) => peer.id)
        }

        if (this.view.state === 'disconnected' && this.view.code === 0) {
          TextInput({ placeholder: ui('Mac LAN IP (manual fallback)', 'Mac 局域网 IP（手动连接）'), text: this.address })
            .height(36).fontSize(14).backgroundColor('#F4F5F7').borderRadius(8)
            .placeholderColor('#8E8E93').onChange((value: string) => { this.address = value; })
          Button(this.view.paired ? ui('Connect using this IP', '通过此 IP 连接') : ui('Pair using this IP', '通过此 IP 配对')).width('100%').height(36)
            .fontSize(14).fontWeight(FontWeight.Medium).fontColor(Color.White)
            .backgroundColor(this.address.trim().length > 0 ? '#1F6B4A' : '#C7C7CC')
            .enabled(this.address.trim().length > 0)
            .onClick(() => this.service.connectManual(this.address))
        }

        if (this.view.code === 0 && (this.view.paired || this.view.state !== 'disconnected')) {
          Row({ space: 8 }) {
            if (this.view.state !== 'disconnected') {
              Button(ui('Disconnect', '断开连接')).layoutWeight(1).height(34).type(ButtonType.Normal)
                .fontSize(13).fontColor('#1C1C1E').backgroundColor('#EFEFF1')
                .onClick(() => this.service.disconnect())
            }
            if (this.view.paired) {
              Button(ui('Unpair', '解除配对')).layoutWeight(1).height(34).type(ButtonType.Normal)
                .fontSize(13).fontColor('#A34329').backgroundColor('#F8EBE6')
                .onClick(() => this.service.unpair())
            }
          }.width('100%')
        }

        Text(ui('Trusted LAN only · unencrypted TCP + token. No input capture, clipboard, files or accessibility service.', '仅限可信局域网 · 当前连接未加密。剪贴板和文件传输尚不支持。'))
          .fontSize(11).fontColor('#8E8E93').lineHeight(15).width('100%')
        }
      }.alignItems(HorizontalAlign.Start).padding({ left: 14, right: 14, top: 10, bottom: 12 }).width('100%').constraintSize({ maxWidth: 540 })
        .onAreaChange((_before: Area, after: Area) => { this.resizePanel(Number(after.height)); })
    }.width('100%').height('100%').scrollBar(BarState.Auto).align(Alignment.Top).backgroundColor(Color.Transparent)
  }
}
