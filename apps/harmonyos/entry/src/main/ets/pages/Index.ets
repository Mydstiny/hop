import { HopService, HopViewState, MacPeer } from '../HopService';

@Entry
@Component
struct Index {
  @State view: HopViewState = HopService.instance.view;
  @State address: string = '';
  private service: HopService = HopService.instance;
  build() {
    Scroll() {
      Column({ space: 10 }) {
        Row() {
          Column({ space: 2 }) {
            Text('Receiver').fontSize(11).fontColor('#8E8E93')
            Text(this.view.state).fontSize(16).fontWeight(FontWeight.Medium).fontColor('#1C1C1E')
          }.alignItems(HorizontalAlign.Start)
          Blank()
          Text(this.view.authorization === 2 ? 'Authorized' : 'Not authorized')
            .fontSize(12)
            .fontColor(this.view.authorization === 2 ? '#1F6B4A' : '#8E8E93')
            .padding({ left: 8, right: 8, top: 4, bottom: 4 })
            .backgroundColor(this.view.authorization === 2 ? '#E7F3EC' : '#F2F2F4')
            .borderRadius(10)
        }.width('100%').alignItems(VerticalAlign.Center)

        if (this.view.message.length > 0) {
          Text(this.view.message).fontSize(13).fontColor('#3A3A3C').width('100%').lineHeight(18)
        }

        if (!this.view.background) {
          Text('Background task / persistent notification not active. Connections are disabled. Relaunch Hop to retry.')
            .fontSize(12).fontColor('#A34329').lineHeight(17).width('100%')
            .padding(10).backgroundColor('#F8EBE6').borderRadius(8)
        }

        Row() {
          Text(this.view.authorization === 2 ? 'System injection authorized' : 'System injection permission required')
            .fontSize(13).fontColor('#1C1C1E').layoutWeight(1)
          if (this.view.authorization !== 2) {
            Button('Authorize')
              .fontSize(13).fontWeight(FontWeight.Medium).fontColor(Color.White)
              .height(32).backgroundColor('#1F6B4A')
              .onClick(() => this.service.authorize())
          }
        }.width('100%').alignItems(VerticalAlign.Center)

        if (this.view.code > 0) {
          Column({ space: 8 }) {
            Text('Compare this code with the Mac').fontSize(12).fontColor('#8E8E93').width('100%')
            Text(this.view.code.toString())
              .fontSize(32).fontWeight(FontWeight.Bold).fontColor('#1C1C1E').letterSpacing(6)
              .width('100%').textAlign(TextAlign.Center)
            Text('Both devices must confirm. No token is saved until the Mac commits.')
              .fontSize(11).fontColor('#8E8E93').lineHeight(15).width('100%')
            Button('Codes match — confirm').width('100%').height(36)
              .fontSize(14).fontWeight(FontWeight.Medium).fontColor(Color.White).backgroundColor('#1F6B4A')
              .onClick(() => this.service.confirm())
            Button('Cancel').width('100%').height(36).type(ButtonType.Normal)
              .fontSize(14).fontColor('#1C1C1E').backgroundColor('#EFEFF1')
              .onClick(() => this.service.disconnect())
          }.width('100%').padding(12).backgroundColor('#F4F5F7').borderRadius(12)
            .alignItems(HorizontalAlign.Start)
        }

        if (this.view.paired && this.view.code === 0) {
          Button('Connect paired Mac').width('100%').height(36)
            .fontSize(14).fontWeight(FontWeight.Medium).fontColor(Color.White).backgroundColor('#1F6B4A')
            .enabled(this.view.state === 'disconnected')
            .onClick(() => this.service.connectBinding())
          Text('Automatically reconnects while enabled and authorized. Disconnect pauses reconnection.')
            .fontSize(11).fontColor('#8E8E93').lineHeight(15).width('100%')
        } else if (this.view.state === 'disconnected' && this.view.code === 0) {
          Text('NEARBY MACS').fontSize(11).fontColor('#8E8E93').width('100%')
          if (this.view.discovery.length > 0) {
            Text(this.view.discovery).fontSize(12).fontColor('#3A3A3C').width('100%')
          }
          ForEach(this.view.peers, (peer: MacPeer) => {
            Button(`${peer.name} · ${peer.host}`).width('100%').height(34)
              .fontSize(13).fontColor('#1C1C1E').backgroundColor('#F4F5F7').type(ButtonType.Normal)
              .onClick(() => this.service.connectPeer(peer))
          }, (peer: MacPeer) => peer.id)
        }

        if (this.view.state === 'disconnected' && this.view.code === 0) {
          TextInput({ placeholder: 'Mac LAN IP (manual fallback)', text: this.address })
            .height(36).fontSize(14).backgroundColor('#F4F5F7').borderRadius(8)
            .placeholderColor('#8E8E93').onChange((value: string) => { this.address = value; })
          Button(this.view.paired ? 'Connect using this IP' : 'Pair using this IP').width('100%').height(36)
            .fontSize(14).fontWeight(FontWeight.Medium).fontColor(Color.White)
            .backgroundColor(this.address.trim().length > 0 ? '#1F6B4A' : '#C7C7CC')
            .enabled(this.address.trim().length > 0)
            .onClick(() => this.service.connectManual(this.address))
        }

        if (this.view.code === 0 && (this.view.paired || this.view.state !== 'disconnected')) {
          Row({ space: 8 }) {
            if (this.view.state !== 'disconnected') {
              Button('Disconnect').layoutWeight(1).height(34).type(ButtonType.Normal)
                .fontSize(13).fontColor('#1C1C1E').backgroundColor('#EFEFF1')
                .onClick(() => this.service.disconnect())
            }
            if (this.view.paired) {
              Button('Unpair').layoutWeight(1).height(34).type(ButtonType.Normal)
                .fontSize(13).fontColor('#A34329').backgroundColor('#F8EBE6')
                .onClick(() => this.service.unpair())
            }
          }.width('100%')
        }

        Text('Trusted LAN only · unencrypted TCP + token. No input capture, clipboard, files or accessibility service.')
          .fontSize(11).fontColor('#8E8E93').lineHeight(15).width('100%')
      }.alignItems(HorizontalAlign.Start).padding({ left: 16, right: 16, top: 8, bottom: 12 }).width('100%')
    }.width('100%').height('100%').scrollBar(BarState.Auto).align(Alignment.TopStart)
  }
}
