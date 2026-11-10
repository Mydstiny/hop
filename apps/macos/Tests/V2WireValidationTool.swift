import Foundation
@main struct V2WireValidationTool {
    static func main() {
        while let line = readLine() {
            let parts = line.split(separator: " ")
            guard parts.count == 4, let kind = UInt8(parts[0]), let seq = UInt64(parts[1]), let epoch = UInt64(parts[2]) else { print("error"); continue }
            let hex = Array(parts[3]); guard hex.count % 2 == 0 else { print("error"); continue }
            var payload = [UInt8](); var ok = true
            for i in stride(from: 0, to: hex.count, by: 2) {
                guard let b = UInt8(String(hex[i...i+1]), radix: 16) else { ok = false; break }
                payload.append(b)
            }
            guard ok else { print("error"); continue }
            print(V2Frame(kind: kind, sequence: seq, epoch: epoch, payload: payload).valid ? "valid" : "invalid")
        }
    }
}
