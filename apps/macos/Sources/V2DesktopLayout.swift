import Foundation
import CoreGraphics

enum V2LayoutEdge: Int, CaseIterable {
    case left, right, top, bottom
    var horizontalBoundary: Bool { self == .top || self == .bottom }
    var opposite: Self { [.right, .left, .bottom, .top][rawValue] }
}

struct V2DesktopLayout {
    struct Key: Hashable, Codable {
        let device: String
        let screen: UInt64
        var local: Bool { device == "local" }
        var valid: Bool { screen > 0 && (local || V2PairOffer.selector(device) != nil) }
    }
    struct Display: Equatable {
        let key: Key
        let label: String
        var frame: CGRect
        let physical: CGRect
    }
    struct Route: Equatable {
        let source: Key
        let target: Key
        let targetFrame: CGRect
        let entryEdge: V2LayoutEdge
        let x, y: UInt16

        var localEntryPoint: CGPoint {
            let px = targetFrame.minX + CGFloat(x) / 65_535 * (targetFrame.width - 1)
            let py = targetFrame.minY + CGFloat(y) / 65_535 * (targetFrame.height - 1)
            return entryEdge.inset(CGPoint(x: px, y: py), within: targetFrame)
        }
    }
    struct Seam {
        let source: Key
        let target: Key
        let edge: V2LayoutEdge
        let span: Range<CGFloat>
        let boundary: CGFloat

        var endpoints: (CGPoint, CGPoint) {
            if edge.horizontalBoundary {
                return (CGPoint(x: span.lowerBound, y: boundary), CGPoint(x: span.upperBound, y: boundary))
            }
            return (CGPoint(x: boundary, y: span.lowerBound), CGPoint(x: boundary, y: span.upperBound))
        }
    }
    struct Placement: Codable {
        let key: Key
        let point: CGPoint
    }

    private(set) var displays: [Display]
    static func frameValid(_ frame: CGRect) -> Bool {
        abs(frame.minX) <= 10_000_000 && abs(frame.minY) <= 10_000_000 &&
            frame.width.isFinite && frame.height.isFinite && (1...65535).contains(frame.width) && (1...65535).contains(frame.height)
    }
    init(_ displays: [Display]) throws {
        guard displays.count <= 128, Set(displays.map(\.key)).count == displays.count,
              displays.allSatisfy({ $0.key.valid && Self.frameValid($0.frame) && Self.frameValid($0.physical) }) else {
            throw WireError.invalid("Invalid display layout")
        }
        for (index, display) in displays.enumerated() {
            if displays.prefix(index).contains(where: { $0.frame.intersection(display.frame).width > 0 && $0.frame.intersection(display.frame).height > 0 }) {
                throw WireError.invalid("Display rectangles overlap")
            }
        }
        self.displays = displays
    }

    /// Newly discovered devices append to the right. Saved disconnected screens
    /// keep their coordinates and may be reused by the same authenticated key.
    static func arrange(_ local: [Display], _ remote: [[Display]], placements: [Key: CGPoint]) throws -> Self {
        var placed = local
        for group in remote {
            guard let left = group.min(by: { $0.physical.minX < $1.physical.minX }) else { continue }
            let right = placed.max(by: { $0.frame.maxX < $1.frame.maxX })?.frame ?? .zero
            // Align the actual touching displays, including a rightmost local display
            // that sits entirely above or below the primary screen.
            let offset = CGPoint(x: right.maxX - left.physical.minX, y: right.minY - left.physical.minY)
            let fallback = group.map { screen in
                var display = screen
                display.frame.origin = CGPoint(x: screen.physical.minX + offset.x, y: screen.physical.minY + offset.y)
                return display
            }
            let next = fallback.map { screen in
                var display = screen
                display.frame.origin = placements[screen.key] ?? screen.frame.origin
                return display
            }
            // A changed resolution can invalidate a previously legal layout.
            // Place that entire computer alongside current ones until edited.
            placed += (try? Self(placed + next)) == nil ? fallback : next
        }
        return try Self(placed)
    }

    func sourceEdge(_ point: CGPoint, margin: CGFloat, movement: CGVector? = nil, matching expected: Route? = nil) -> Route? {
        guard let source = displays.first(where: { $0.key.local && $0.physical.contains(point) }) else { return nil }
        if let expected, source.key != expected.source { return nil }
        let b = source.physical
        let candidates: [(V2LayoutEdge, CGFloat, CGFloat)] = [
            (.left, point.x - b.minX, point.y - b.minY),
            (.right, b.maxX - 1 - point.x, point.y - b.minY),
            (.top, point.y - b.minY, point.x - b.minX),
            (.bottom, b.maxY - 1 - point.y, point.x - b.minX)
        ]
        for (edge, distance, along) in candidates.sorted(by: { ($0.1, $0.0.rawValue) < ($1.1, $1.0.rawValue) }) where distance <= margin {
            if let movement, !edge.isOutward(movement) { continue }
            if let expected, edge.opposite != expected.entryEdge { continue }
            if let route = crossing(from: source, edge: edge, offset: along), !route.target.local,
               expected == nil || route.target == expected?.target { return route }
        }
        return nil
    }

    func remoteEdge(_ key: Key, x: UInt16, y: UInt16, accepts: (V2LayoutEdge) -> Bool = { _ in true }) -> Route? {
        guard let source = displays.first(where: { $0.key == key }) else { return nil }
        for edge in V2LayoutEdge.allCases where edge.isBoundary(x: x, y: y) && accepts(edge) {
            let size = edge.horizontalBoundary ? source.frame.width : source.frame.height
            let along = edge.horizontalBoundary ? x : y
            // Input and injection both use the first through last addressable pixel.
            // Round back to that same pixel before deciding which half-open seam owns it.
            if let route = crossing(from: source, edge: edge, offset: pixel(along, dimension: size)) { return route }
        }
        return nil
    }

    func crossing(from key: Key, edge: V2LayoutEdge, along: CGFloat) -> Route? {
        guard along.isFinite, (0...1).contains(along),
              let source = displays.first(where: { $0.key == key }) else { return nil }
        let length = edge.horizontalBoundary ? source.frame.width : source.frame.height
        return crossing(from: source, edge: edge, offset: max(0, length - 1) * along)
    }

    private func crossing(from source: Display, edge: V2LayoutEdge, offset: CGFloat) -> Route? {
        let sourceStart = edge.horizontalBoundary ? source.frame.minX : source.frame.minY
        let at = sourceStart + offset
        let key = source.key
        for target in displays where target.key != key {
            guard let seam = Self.seam(from: source, to: target, edge: edge), seam.span.contains(at) else { continue }
            let start = edge.horizontalBoundary ? target.frame.minX : target.frame.minY
            let length = edge.horizontalBoundary ? target.frame.width : target.frame.height
            let a = normalized(at - start, dimension: length)
            let across = edge.horizontalBoundary ? target.frame.height : target.frame.width
            let inset = normalized(min(2, max(0, across - 1)), dimension: across)
            let x: UInt16
            let y: UInt16
            switch edge.opposite {
            case .left: (x, y) = (inset, a)
            case .right: (x, y) = (UInt16.max - inset, a)
            case .top: (x, y) = (a, inset)
            case .bottom: (x, y) = (a, UInt16.max - inset)
            }
            return Route(source: key, target: target.key, targetFrame: target.physical, entryEdge: edge.opposite, x: x, y: y)
        }
        return nil
    }
    private static func seam(from a: Display, to b: Display, edge: V2LayoutEdge) -> Seam? {
        let boundary = edge.boundary(of: a.frame)
        guard boundary == edge.opposite.boundary(of: b.frame) else { return nil }
        let first = edge.along(a.frame)
        let second = edge.along(b.frame)
        let start = max(first.lowerBound, second.lowerBound)
        let end = min(first.upperBound, second.upperBound)
        guard start < end else { return nil }
        return Seam(source: a.key, target: b.key, edge: edge, span: start..<end, boundary: boundary)
    }

    var seams: [Seam] {
        displays.enumerated().flatMap { index, a in
            displays.dropFirst(index + 1).compactMap { b in
                V2LayoutEdge.allCases.lazy.compactMap { Self.seam(from: a, to: b, edge: $0) }.first
            }
        }
    }

    func unreachable(using seams: [Seam]) -> Set<Key> {
        var graph: [Key: [Key]] = [:]
        for seam in seams {
            graph[seam.source, default: []].append(seam.target)
            graph[seam.target, default: []].append(seam.source)
        }
        var queue = displays.filter { $0.key.local }.map(\.key)
        var reached = Set(queue)
        while let key = queue.popLast() {
            for neighbor in graph[key, default: []] where reached.insert(neighbor).inserted { queue.append(neighbor) }
        }
        return Set(displays.map(\.key)).subtracting(reached)
    }

    /// Move an entire selected device or one expanded screen to an actual side of the anchor.
    func placing(_ moving: Set<Key>, by anchor: Key, on edge: V2LayoutEdge) -> Self? {
        guard !moving.isEmpty, moving.allSatisfy({ !$0.local }), let fixed = displays.first(where: { $0.key == anchor }) else { return nil }
        let opposite = edge.opposite
        let screens = displays.filter { moving.contains($0.key) }
        let bounds = screens.reduce(CGRect.null) { $0.union($1.frame) }
        let nearest = opposite.boundary(of: bounds)
        guard let touching = screens.first(where: { opposite.boundary(of: $0.frame) == nearest }) else { return nil }
        let across = edge.boundary(of: fixed.frame) - nearest
        let fixedSpan = edge.along(fixed.frame), movingSpan = edge.along(touching.frame)
        let center = ((fixedSpan.lowerBound + fixedSpan.upperBound - movingSpan.lowerBound - movingSpan.upperBound) / 2).rounded()
        let offsets = [center, fixedSpan.lowerBound - movingSpan.lowerBound, fixedSpan.upperBound - movingSpan.upperBound]
        for along in offsets {
            let shifted = displays.map { screen in
                guard moving.contains(screen.key) else { return screen }
                var screen = screen
                screen.frame = screen.frame.offsetBy(dx: edge.horizontalBoundary ? along : across,
                                                     dy: edge.horizontalBoundary ? across : along)
                return screen
            }
            if let layout = try? Self(shifted) { return layout }
        }
        return nil
    }

    static func load(_ data: Data?) -> [Key: CGPoint]? {
        guard let data, data.count <= 64 * 1024, let values = try? JSONDecoder().decode([Placement].self, from: data),
              values.count <= 128, Set(values.map(\.key)).count == values.count,
              values.allSatisfy({ !$0.key.local && $0.key.valid && abs($0.point.x) <= 10_000_000 && abs($0.point.y) <= 10_000_000 }) else { return nil }
        return Dictionary(uniqueKeysWithValues: values.map { ($0.key, $0.point) })
    }
    func encode(keeping old: [Key: CGPoint]) throws -> Data {
        let current = displays.filter { !$0.key.local }.map { Placement(key: $0.key, point: $0.frame.origin) }
        let keys = Set(current.map(\.key))
        let saved = old.filter { !keys.contains($0.key) }
            .sorted { ($0.key.device, $0.key.screen) < ($1.key.device, $1.key.screen) }
            .prefix(max(0, 128 - current.count))
            .map { Placement(key: $0.key, point: $0.value) }
        return try JSONEncoder().encode(current + saved)
    }
}

extension V2LayoutEdge {
    fileprivate func along(_ frame: CGRect) -> Range<CGFloat> {
        horizontalBoundary ? frame.minX..<frame.maxX : frame.minY..<frame.maxY
    }
    fileprivate func boundary(of frame: CGRect) -> CGFloat {
        switch self {
        case .left: return frame.minX
        case .right: return frame.maxX
        case .top: return frame.minY
        case .bottom: return frame.maxY
        }
    }
    fileprivate func isBoundary(x: UInt16, y: UInt16) -> Bool {
        switch self {
        case .left: return x == 0
        case .right: return x == .max
        case .top: return y == 0
        case .bottom: return y == .max
        }
    }
    fileprivate func isOutward(_ motion: CGVector) -> Bool {
        switch self {
        case .left: return motion.dx < 0
        case .right: return motion.dx > 0
        case .top: return motion.dy < 0
        case .bottom: return motion.dy > 0
        }
    }
    func inset(_ point: CGPoint, within frame: CGRect) -> CGPoint {
        let horizontalInset = min(8, max(0, (frame.width - 1) / 2))
        let verticalInset = min(8, max(0, (frame.height - 1) / 2))
        switch self {
        case .left: return CGPoint(x: frame.minX + horizontalInset, y: point.y)
        case .right: return CGPoint(x: frame.maxX - 1 - horizontalInset, y: point.y)
        case .top: return CGPoint(x: point.x, y: frame.minY + verticalInset)
        case .bottom: return CGPoint(x: point.x, y: frame.maxY - 1 - verticalInset)
        }
    }
}
