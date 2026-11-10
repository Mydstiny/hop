import Foundation
import Darwin

@main struct V2SelectorReaderTests {
    static func main() throws {
        var checks = 0
        func check(_ ok: Bool, _ message: String) { checks += 1; if !ok { fputs("FAIL v2 selector: \(message)\n",stderr); exit(1) } }
        let prefix: [UInt8] = [0x48,0x50,0x53,0x4c]
        func pair() -> [Int32] {
            var fds: [Int32] = [-1,-1]
            check(socketpair(AF_UNIX, SOCK_STREAM, 0, &fds) == 0, "socket pair")
            check(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0 && fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0, "nonblocking descriptor")
            return fds
        }
        let fds = pair(), completed = DispatchSemaphore(value: 0), lock = NSLock()
        var result: String?, remaining: [UInt8] = []
        guard let reader = V2SelectorReader(fd: fds[0], prefix: prefix, completion: { value in
            if case .success(let selected) = value {
                var tail = [UInt8](repeating: 0, count: 3)
                let n = tail.withUnsafeMutableBytes { recv(selected.fd, $0.baseAddress, 3, 0) }
                Darwin.close(selected.fd)
                lock.lock(); result = selected.fingerprint; remaining = n == 3 ? tail : []; lock.unlock()
            }
            completed.signal()
        }) else { fatalError("could not create reader") }
        reader.start()
        let packet = Array(String(repeating: "A", count: 64).utf8) + [1,2,3]
        let n = packet.withUnsafeBytes { send(fds[1], $0.baseAddress, packet.count, 0) }
        check(n == packet.count && completed.wait(timeout: .now() + .seconds(2)) == .success, "selector received")
        lock.lock(); let observed = result, tail = remaining; lock.unlock()
        check(observed == String(repeating: "A", count: 64) && tail == [1,2,3], "fingerprint parsed without consuming TLS bytes")
        Darwin.close(fds[1]); reader.cancel()
        let invalidFds = pair(), rejected = DispatchSemaphore(value: 0)
        var rejectedInvalid = false
        guard let invalid = V2SelectorReader(fd: invalidFds[0], prefix: prefix, completion: { value in
            if case .failure = value { lock.lock(); rejectedInvalid = true; lock.unlock() }; rejected.signal()
        }) else { fatalError("could not create invalid reader") }
        invalid.start()
        let bad = Array(String(repeating: "g", count: 64).utf8)
        let sent = bad.withUnsafeBytes { send(invalidFds[1], $0.baseAddress, bad.count, 0) }
        check(sent == bad.count && rejected.wait(timeout: .now() + .seconds(2)) == .success, "invalid selector completes")
        lock.lock(); let denied = rejectedInvalid; lock.unlock()
        check(denied, "non-hex selector rejected")
        Darwin.close(invalidFds[1]); invalid.cancel()
        print("PASS \(checks) v2 selector checks; untrusted hints only, no TLS or input")
    }
}
