import Foundation
import Network
@main struct TCPOptionsTests {
    static func main() {
        let options = TCPServer.transportParameters().defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options
        guard options?.noDelay == true else { fputs("FAIL: TCP_NODELAY not configured\n", stderr); exit(1) }
        print("PASS: Mac input transport configures TCP noDelay; no network or input used")
    }
}
