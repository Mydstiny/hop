import Foundation
import Darwin

@main struct V2OfferExchangeTests {
    static func main() throws {
        var checks = 0
        func check(_ ok: Bool, _ message: String) { checks += 1; if !ok { fputs("FAIL v2 offer: \(message)\n",stderr); exit(1) } }
        let localPEM = try String(contentsOfFile: CommandLine.arguments[1], encoding: .utf8)
        let remotePEM = try String(contentsOfFile: CommandLine.arguments[2], encoding: .utf8)
        let local = V2PairOffer(role: 1, nonce: Array(repeating: 1, count: 16), name: "Mac", certificatePEM: localPEM)
        let remote = V2PairOffer(role: 2, nonce: Array(repeating: 2, count: 16), name: "Harmony", certificatePEM: remotePEM)
        guard let remoteBytes = remote.encoded() else { fatalError("invalid fixture") }
        var fds: [Int32] = [-1,-1]
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, &fds) == 0, "socket pair")
        check(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0 && fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0, "configure worker descriptor")
        let completed = DispatchSemaphore(value: 0), lock = NSLock()
        var result: Result<V2PairOffer, Error>?
        guard let exchange = V2OfferExchange(fd: fds[0], prefix: Array(remoteBytes[0..<4]), local: local, completion: { value in
            lock.lock(); result = value; lock.unlock(); completed.signal()
        }) else { fatalError("could not create exchange") }
        exchange.start()
        let body = Array(remoteBytes.dropFirst(4))
        let first = Array(body.prefix(5)), second = Array(body.dropFirst(5))
        func write(_ bytes: [UInt8]) -> Int { bytes.withUnsafeBytes { send(fds[1], $0.baseAddress, bytes.count, 0) } }
        check(write(first) == first.count && write(second) == second.count && shutdown(fds[1], SHUT_WR) == 0, "send fragmented candidate and half-close")
        var reply: [UInt8] = []; var poller = pollfd(fd: fds[1], events: Int16(POLLIN), revents: 0)
        for _ in 0..<100 {
            let status = poll(&poller, 1, 50)
            if status > 0 {
                var buf = [UInt8](repeating: 0, count: 4096)
                let n = buf.withUnsafeMutableBytes { recv(fds[1], $0.baseAddress, 4096, 0) }
                if n <= 0 { break }; reply += buf.prefix(n)
            }
        }
        Darwin.close(fds[1])
        check(completed.wait(timeout: .now() + .seconds(2)) == .success, "exchange completed")
        lock.lock(); let final = result; lock.unlock()
        if case .success(let offer) = final { check(offer == remote, "received candidate only") }
        else { check(false, "successful candidate") }
        let decoder = V2PairOfferDecoder()
        check(decoder.feed(reply) && decoder.finish() == local, "public local offer returned")
        exchange.cancel()
        var sameFds: [Int32] = [-1,-1]
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, &sameFds) == 0, "self-candidate socket pair")
        check(fcntl(sameFds[0], F_SETFL, O_NONBLOCK) == 0 && fcntl(sameFds[0], F_SETFD, FD_CLOEXEC) == 0, "self-candidate descriptor")
        let same = V2PairOffer(role: 2, nonce: Array(repeating: 3, count: 16), name: "Other", certificatePEM: localPEM).encoded()!
        let rejected = DispatchSemaphore(value: 0)
        var didReject = false
        guard let rejectExchange = V2OfferExchange(fd: sameFds[0], prefix: Array(same[0..<4]), local: local, completion: { value in
            if case .failure = value { lock.lock(); didReject = true; lock.unlock() }; rejected.signal()
        }) else { fatalError("could not create rejection exchange") }
        rejectExchange.start()
        let sameBody = Array(same.dropFirst(4))
        let written = sameBody.withUnsafeBytes { send(sameFds[1], $0.baseAddress, sameBody.count, 0) }
        check(written == sameBody.count && shutdown(sameFds[1], SHUT_WR) == 0 && rejected.wait(timeout: .now() + .seconds(2)) == .success, "same identity exchange completes")
        lock.lock(); let denied = didReject; lock.unlock()
        check(denied, "same certificate cannot become a peer")
        var response = [UInt8](repeating: 0, count: 1)
        let read = response.withUnsafeMutableBytes { recv(sameFds[1], $0.baseAddress, 1, 0) }
        check(read == 0, "rejected candidate gets no offer response")
        Darwin.close(sameFds[1]); rejectExchange.cancel()
        var extraFds: [Int32] = [-1,-1]
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, &extraFds) == 0, "trailing-data socket pair")
        check(fcntl(extraFds[0], F_SETFL, O_NONBLOCK) == 0 && fcntl(extraFds[0], F_SETFD, FD_CLOEXEC) == 0, "trailing-data descriptor")
        let extraDone = DispatchSemaphore(value: 0); var extraRejected = false
        guard let extraExchange = V2OfferExchange(fd: extraFds[0], prefix: Array(remoteBytes[0..<4]), local: local, completion: { value in
            if case .failure = value { lock.lock(); extraRejected = true; lock.unlock() }; extraDone.signal()
        }) else { fatalError("could not create trailing-data exchange") }
        extraExchange.start()
        let extraBody = Array(remoteBytes.dropFirst(4))
        let extraWritten = extraBody.withUnsafeBytes { send(extraFds[1], $0.baseAddress, extraBody.count, 0) }
        check(extraWritten == extraBody.count && extraDone.wait(timeout: .now() + .milliseconds(100)) == .timedOut,
              "candidate is not accepted before peer half-closes")
        var lateByte: UInt8 = 0x55
        check(send(extraFds[1], &lateByte, 1, 0) == 1 && shutdown(extraFds[1], SHUT_WR) == 0 && extraDone.wait(timeout: .now() + .seconds(2)) == .success, "late trailing-data exchange completes")
        lock.lock(); let deniedExtra = extraRejected; lock.unlock()
        check(deniedExtra, "trailing byte is rejected")
        var rejectedResponse: UInt8 = 0
        check(recv(extraFds[1], &rejectedResponse, 1, 0) == 0, "trailing data gets no candidate response")
        Darwin.close(extraFds[1]); extraExchange.cancel()
        let networkDone = DispatchSemaphore(value: 0)
        var networkExchange: V2OfferExchange?, networkCandidate: V2PairOffer?
        let listener = V2IncomingListener { fd, kind, prefix in
            guard kind == .offer else { Darwin.close(fd); networkDone.signal(); return }
            guard let job = V2OfferExchange(fd: fd, prefix: prefix, local: local, completion: { value in
                if case .success(let offer) = value { lock.lock(); networkCandidate = offer; lock.unlock() }
                networkDone.signal()
            }) else { networkDone.signal(); return }
            lock.lock(); networkExchange = job; lock.unlock(); job.start()
        }
        let port = try listener.start(port: 0); listener.openPairingWindow()
        let client = socket(AF_INET, SOCK_STREAM, 0)
        var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET); address.sin_port = port.bigEndian; address.sin_addr.s_addr = in_addr_t(0x7f000001).bigEndian
        let connected = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(client, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        check(client >= 0 && connected == 0, "candidate listener loopback")
        let sentNetwork = remoteBytes.withUnsafeBytes { send(client, $0.baseAddress, remoteBytes.count, 0) }
        check(sentNetwork == remoteBytes.count && shutdown(client, SHUT_WR) == 0, "send complete candidate over TCP and half-close")
        var networkReply: [UInt8] = []
        for _ in 0..<100 {
            var p = pollfd(fd: client, events: Int16(POLLIN), revents: 0)
            if poll(&p, 1, 50) > 0 {
                var buf = [UInt8](repeating: 0, count: 4096)
                let n = buf.withUnsafeMutableBytes { recv(client, $0.baseAddress, 4096, 0) }
                if n <= 0 { break }; networkReply += buf.prefix(n)
            }
        }
        Darwin.close(client); listener.stop()
        check(networkDone.wait(timeout: .now() + .seconds(2)) == .success, "listener and candidate exchange completed")
        lock.lock(); let networkResult = networkCandidate; let heldJob = networkExchange; lock.unlock()
        let networkDecoder = V2PairOfferDecoder()
        check(networkResult == remote && heldJob != nil && networkDecoder.feed(networkReply) && networkDecoder.finish() == local,
              "TCP public candidate exchange is bidirectional")
        print("PASS \(checks) public v2 offer exchange checks; no trust, TLS or system input")
    }
}
