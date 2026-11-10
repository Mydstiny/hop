import Foundation
import Darwin

@main struct V2IncomingListenerTests {
    static func main() throws {
        var checks = 0
        func check(_ value: Bool, _ label: String) {
            checks += 1
            if !value { fputs("FAIL v2 listener: \(label)\n", stderr); exit(1) }
        }
        let lock = NSLock(), received = DispatchSemaphore(value: 0)
        var handoffs: [(V2IncomingListener.Kind, [UInt8])] = []
        let listener = V2IncomingListener { fd, kind, prefix in
            Darwin.close(fd)
            lock.lock(); handoffs.append((kind, prefix)); lock.unlock(); received.signal()
        }
        let port = try listener.start(port: 0)
        check(port != 0, "ephemeral listener port")
        func client() -> Int32 {
            let fd = socket(AF_INET, SOCK_STREAM, 0)
            var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            address.sin_family = sa_family_t(AF_INET); address.sin_port = port.bigEndian; address.sin_addr.s_addr = in_addr_t(0x7f000001).bigEndian
            let status = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
            check(fd >= 0 && status == 0, "connect loopback")
            return fd
        }
        func write(_ fd: Int32, _ bytes: [UInt8]) {
            let n = bytes.withUnsafeBytes { send(fd, $0.baseAddress, bytes.count, 0) }
            check(n == bytes.count, "write prefix")
        }
        let selector = client(); write(selector, [0x48,0x50]);
        check(received.wait(timeout: .now() + .milliseconds(50)) == .timedOut, "fragmented prefix waits")
        write(selector, [0x53,0x4c]); check(received.wait(timeout: .now() + .seconds(2)) == .success, "selector classified")
        Darwin.close(selector)
        lock.lock(); let first = handoffs.first; lock.unlock()
        check(first?.0 == .selector && first?.1 == [0x48,0x50,0x53,0x4c], "handoff includes selector prefix")
        let denied = client(); write(denied, [0x48,0x50,0x4f,0x50])
        check(received.wait(timeout: .now() + .milliseconds(100)) == .timedOut, "offer denied outside pairing window")
        Darwin.close(denied)
        listener.openPairingWindow(seconds: 2)
        let offer = client(); write(offer, [0x48,0x50,0x4f,0x50])
        check(received.wait(timeout: .now() + .seconds(2)) == .success, "offer classified during pairing")
        Darwin.close(offer)
        lock.lock(); let last = handoffs.last; lock.unlock()
        check(last?.0 == .offer && last?.1 == [0x48,0x50,0x4f,0x50], "handoff includes offer prefix")
        let unknown = client(); write(unknown, [0x42,0x41,0x44,0x21])
        check(received.wait(timeout: .now() + .milliseconds(100)) == .timedOut, "unknown preface rejected even in pairing")
        Darwin.close(unknown)
        listener.openPairingWindow(seconds: 0.05); Thread.sleep(forTimeInterval: 0.1)
        let expired = client(); write(expired, [0x48,0x50,0x4f,0x50])
        check(received.wait(timeout: .now() + .milliseconds(100)) == .timedOut, "expired pairing window rejects offer")
        Darwin.close(expired)
        listener.closePairingWindow(); listener.stop(); listener.stop()
        print("PASS \(checks) v2 incoming listener loopback checks; no TLS, app or input")
    }
}
