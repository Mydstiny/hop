import Foundation
import Network

/// All socket work is off the event-tap/main thread. No synchronous network calls exist.
final class TCPServer {
    private let queue = DispatchQueue(label: "local.hop.tcp", qos: .userInteractive)
    private var listener: NWListener?
    private var connection: NWConnection?
    private var connectionID: UUID?
    private var decoder = FrameDecoder()
    private var pendingWrites = 0
    private var faulted = false
    var onAccept: ((UUID) -> Void)?
    var onMessage: ((UUID, Message) -> Void)?
    var onFault: ((UUID?, String) -> Void)?
    var onListening: ((UInt16) -> Void)?

    static func transportParameters() -> NWParameters {
        let options = NWProtocolTCP.Options()
        options.noDelay = true // small input frames should not wait for Nagle aggregation
        return NWParameters(tls: nil, tcp: options)
    }
    func start(identity: [UInt8], port: UInt16 = 24890, loopbackOnly: Bool = false) {
        queue.async { [weak self] in
            guard let self else { return }
            do {
                let parameters = Self.transportParameters()
                parameters.allowLocalEndpointReuse = true
                if loopbackOnly { parameters.requiredLocalEndpoint = .hostPort(host: .ipv4(.loopback), port: NWEndpoint.Port(rawValue: port)!) }
                let listener = try NWListener(using: parameters, on: NWEndpoint.Port(rawValue: port)!)
                self.listener = listener
                let uuid = identity.withUnsafeBufferPointer { NSUUID(uuidBytes: $0.baseAddress!).uuidString }
                let txt = NetService.data(fromTXTRecord: ["v": Data("1".utf8), "id": Data(uuid.utf8)])
                if !loopbackOnly { listener.service = NWListener.Service(name: Host.current().localizedName ?? "Hop Mac", type: "_hop._tcp", txtRecord: txt) }
                listener.newConnectionHandler = { [weak self] connection in self?.accept(connection) }
                listener.stateUpdateHandler = { [weak self, weak listener] state in
                    guard let self else { return }
                    switch state {
                    case .ready:
                        let bound = listener?.port?.rawValue ?? port
                        DispatchQueue.main.async { self.onListening?(bound) }
                    case .failed(let error): self.reportFault("Listener: \(error)", id: nil)
                    default: break
                    }
                }
                listener.start(queue: self.queue)
            } catch { self.reportFault("Cannot listen: \(error)", id: nil) }
        }
    }
    private func accept(_ connection: NWConnection) {
        guard self.connection == nil else { connection.cancel(); return }
        self.connection = connection
        let id = UUID(); connectionID = id; decoder = FrameDecoder(); faulted = false; pendingWrites = 0
        // Publish ownership before .ready/.waiting/.failed so every early failure can be restored/closed.
        DispatchQueue.main.async { self.onAccept?(id) }
        connection.stateUpdateHandler = { [weak self] state in
            guard let self, self.connectionID == id else { return }
            switch state {
            case .ready:
                self.receive(id)
            case .failed(let error): self.reportFault("Network: \(error)", id: id)
            case .waiting(let error): self.reportFault("Network unavailable: \(error)", id: id)
            default: break
            }
        }
        connection.start(queue: queue)
    }
    private func receive(_ id: UUID) {
        guard connectionID == id, !faulted else { return }
        connection?.receive(minimumIncompleteLength: 1, maximumLength: 16384) { [weak self] data, _, eof, error in
            guard let self, self.connectionID == id, !self.faulted else { return }
            if let data, !data.isEmpty {
                do {
                    let messages = try self.decoder.feed(data)
                    for message in messages { DispatchQueue.main.async { self.onMessage?(id, message) } }
                } catch { self.reportFault("Protocol: \(error)", id: id); return }
            }
            if let error { self.reportFault("Receive: \(error)", id: id); return }
            if eof { self.reportFault(self.decoder.hasPartialFrame ? "Truncated frame / EOF" : "Peer disconnected", id: id); return }
            self.receive(id)
        }
    }
    func send(_ message: Message, id: UUID) {
        queue.async { [weak self] in
            guard let self, self.connectionID == id, !self.faulted else { return }
            guard self.pendingWrites < 256 else { self.reportFault("Slow peer: bounded write queue exceeded", id: id); return }
            self.pendingWrites += 1
            self.connection?.send(content: message.frame, completion: .contentProcessed { [weak self] error in
                guard let self, self.connectionID == id else { return }
                self.pendingWrites = max(0, self.pendingWrites - 1)
                if let error { self.reportFault("Send: \(error)", id: id) }
            })
        }
    }
    private func reportFault(_ text: String, id: UUID?) {
        if id != nil { guard !faulted else { return }; faulted = true }
        // Deliberately do NOT cancel here: owner restores local input FIRST on main.
        DispatchQueue.main.async { [weak self] in self?.onFault?(id, text) }
    }
    func close(id: UUID, final: Message? = nil) {
        queue.async { [weak self] in
            guard let self, self.connectionID == id, let connection = self.connection else { return }
            self.connection = nil; self.connectionID = nil; self.faulted = false
            if let final {
                connection.send(content: final.frame, completion: .contentProcessed { _ in connection.cancel() })
                self.queue.asyncAfter(deadline: .now() + .milliseconds(100)) { connection.cancel() }
            } else { connection.cancel() }
        }
    }
    func stop() {
        queue.async { [weak self] in
            self?.connection?.cancel(); self?.connection = nil; self?.connectionID = nil
            self?.listener?.cancel(); self?.listener = nil
        }
    }
}
