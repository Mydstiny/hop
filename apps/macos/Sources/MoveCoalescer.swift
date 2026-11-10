import Foundation

/// One-slot queue: movement alone may coalesce. Caller flushes before every nonmove and flushes at the selected forwarding-rate limit.
struct MoveCoalescer {
    private(set) var pending: Message?
    mutating func offer(_ message: Message) {
        precondition(message.type == .move)
        pending = message
    }
    mutating func take() -> Message? { defer { pending = nil }; return pending }
    mutating func clear() { pending = nil }
}

protocol MoveTimerToken: AnyObject { func invalidate() }
extension Timer: MoveTimerToken {}

/// Main-run-loop scheduler: no high-frequency timer exists while idle. Posting
/// timer creation keeps it outside the event-tap callback. Only movement may
/// coalesce; the capture path still flushes before every non-movement event.
final class MoveForwardingScheduler {
    typealias TimerFactory = (TimeInterval, @escaping () -> Void) -> MoveTimerToken
    private let hasPending: () -> Bool
    private let flush: () -> Void
    private let rate: () -> Int
    private let post: (@escaping () -> Void) -> Void
    private let makeTimer: TimerFactory
    private var timer: MoveTimerToken?
    private var queued = false
    private var stopped = false
    init(hasPending: @escaping () -> Bool, flush: @escaping () -> Void, rate: @escaping () -> Int,
         post: @escaping (@escaping () -> Void) -> Void = { DispatchQueue.main.async(execute: $0) },
         makeTimer: @escaping TimerFactory = { interval, action in
             let timer = Timer(timeInterval: interval, repeats: true) { _ in action() }
             RunLoop.main.add(timer, forMode: .common)
             return timer
         }) {
        self.hasPending = hasPending; self.flush = flush; self.rate = rate; self.post = post; self.makeTimer = makeTimer
    }
    deinit { timer?.invalidate() }
    func movementAvailable() {
        guard !stopped && !queued && timer == nil else { return }
        queued = true
        post { [weak self] in
            guard let self else { return }
            self.queued = false
            self.scheduleIfNeeded()
        }
    }
    func rateChanged() {
        timer?.invalidate(); timer = nil
        if !stopped { scheduleIfNeeded() }
    }
    func stop() { stopped = true; timer?.invalidate(); timer = nil }
    private func scheduleIfNeeded() {
        guard !stopped && timer == nil && hasPending() else { return }
        let hz = rate()
        guard [60, 120, 240, 500, 1000].contains(hz) else { return }
        timer = makeTimer(1.0 / Double(hz)) { [weak self] in self?.tick() }
    }
    private func tick() {
        guard !stopped else { return }
        guard hasPending() else { timer?.invalidate(); timer = nil; return }
        flush()
    }
}
