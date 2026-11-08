import { HopService, HopViewState, MacPeer } from '../HopService';

@Entry
@Component
struct Index {
  @State view: HopViewState = HopService.instance.view;
  @State address: string = '';
  private service: HopService = HopService.instance;
  build() {
    Scroll() {
      Column({ space: 12 }) {
        Row() {
          Text('Hop').fontSize(26).fontWeight(FontWeight.Bold)
          Blank()
          Text('RECEIVE ONLY').fontSize(10).fontColor('#567568')
        }.width('100%')
        Text(this.view.state.toUpperCase()).fontSize(11).fontWeight(FontWeight.Medium).fontColor('#46735F').width('100%')
        Text(this.view.message).fontSize(14).fontColor('#444444').width('100%')
        Divider()
        if (!this.view.background) {
          Text('Background task / persistent notification not active. Connections are disabled. Relaunch Hop to retry.').fontSize(12).fontColor('#A34329')
        }
        Row() {
          Text(this.view.authorization === 2 ? 'System injection authorized' : 'System injection permission required').fontSize(13)
          Blank()
          if (this.view.authorization !== 2) {
            Button('Authorize').fontSize(12).onClick(() => this.service.authorize())
          }
        }.width('100%')
        if (this.view.code > 0) {
          Text('Compare this code with the Mac').fontSize(12)
          Text(this.view.code.toString()).fontSize(36).fontWeight(FontWeight.Bold).letterSpacing(5)
          Button('Codes match — confirm').width('100%').onClick(() => this.service.confirm())
          Button('Cancel').type(ButtonType.Normal).onClick(() => this.service.disconnect())
          Text('Both devices must confirm. No token is saved until the Mac commits.').fontSize(11).fontColor('#777777')
        }
        if (this.view.paired) {
          Button('Connect paired Mac').width('100%').enabled(this.view.state === 'disconnected').onClick(() => this.service.connectBinding())
          Text('Automatically reconnects while enabled and authorized. Disconnect pauses reconnection.').fontSize(11).fontColor('#777777')
        } else if (this.view.state === 'disconnected') {
          Text('NEARBY MACS').fontSize(11).fontColor('#777777').width('100%')
          Text(this.view.discovery).fontSize(12).width('100%')
          ForEach(this.view.peers, (peer: MacPeer) => {
            Button(`${peer.name} · ${peer.host}`).width('100%').fontSize(13).onClick(() => this.service.connectPeer(peer))
          }, (peer: MacPeer) => peer.id)
        }
        if (this.view.state === 'disconnected') {
          TextInput({ placeholder: 'Mac LAN IP (manual fallback)', text: this.address })
            .onChange((value: string) => { this.address = value; })
          Button(this.view.paired ? 'Connect using this IP' : 'Pair using this IP').width('100%')
            .enabled(this.address.trim().length > 0).onClick(() => this.service.connectManual(this.address))
        }
        Row({ space: 12 }) {
          Button('Disconnect').type(ButtonType.Normal).fontSize(12).onClick(() => this.service.disconnect())
          if (this.view.paired) {
            Button('Unpair').type(ButtonType.Normal).fontSize(12).fontColor('#A34329').onClick(() => this.service.unpair())
          }
        }
        Divider()
        Text('Trusted LAN only · unencrypted TCP + token\nNo input capture, clipboard, files or accessibility service.').fontSize(11).fontColor('#777777').width('100%')
      }.alignItems(HorizontalAlign.Start).padding(20).width('100%')
    }.width('100%').height('100%').backgroundColor('#FAFAF8')
  }
}
