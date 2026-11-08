import Foundation

/// One-slot queue: movement alone may coalesce. Caller flushes before every nonmove and ticks at 120 Hz.
struct MoveCoalescer {
    private(set) var pending: Message?
    mutating func offer(_ message: Message) {
        precondition(message.type == .move)
        pending = message
    }
    mutating func take() -> Message? { defer { pending = nil }; return pending }
    mutating func clear() { pending = nil }
}
