import Foundation
import CoreGraphics

@main enum V2DesktopLayoutTests {
    static func main() throws {
        var checks = 0
        func check(_ passed: @autoclosure () -> Bool, _ what: String) throws {
            guard passed() else { throw WireError.invalid("Layout: " + what) }
            checks += 1
        }
        let local = V2DesktopLayout.Key(device: "local", screen: 1)
        let a = V2DesktopLayout.Key(device: String(repeating: "A", count: 64), screen: 7)
        let b = V2DesktopLayout.Key(device: String(repeating: "B", count: 64), screen: 7)
        let c = V2DesktopLayout.Key(device: String(repeating: "C", count: 64), screen: 9)
        let d = V2DesktopLayout.Key(device: String(repeating: "C", count: 64), screen: 10)
        func display(_ key: V2DesktopLayout.Key, x: Int, y: Int, w: Int, h: Int) -> V2DesktopLayout.Display {
            let frame = CGRect(x: x, y: y, width: w, height: h)
            return .init(key: key, label: key.device, frame: frame, physical: frame)
        }
        let localScreen = display(local, x: 0, y: 0, w: 1000, h: 800)
        let first = display(a, x: 0, y: 0, w: 640, h: 480)
        let second = display(b, x: 0, y: 0, w: 640, h: 480)
        let top = display(c, x: 0, y: 0, w: 500, h: 400)
        let top2 = display(d, x: 500, y: 0, w: 500, h: 400)
        let placements: [V2DesktopLayout.Key: CGPoint] = [
            a: .init(x: 1000, y: 200), b: .init(x: 1640, y: 200), c: .init(x: 0, y: -400), d: .init(x: 500, y: -400)
        ]
        let layout = try V2DesktopLayout.arrange([localScreen], [[first], [second], [top, top2]], placements: placements)
        try check(layout.sourceEdge(CGPoint(x: 999, y: 350), margin: 6)?.target == a, "local to an offset right display")
        try check(layout.sourceEdge(CGPoint(x: 999, y: 99), margin: 6) == nil, "uncovered edge stays local")
        try check(layout.sourceEdge(CGPoint(x: 995, y: 350), margin: 6)?.entryEdge == .left, "right exit enters left")
        try check(layout.sourceEdge(CGPoint(x: 650, y: 2), margin: 6)?.target == d, "upper edge to correct display of a grouped computer")
        try check(layout.sourceEdge(CGPoint(x: 250, y: 2), margin: 6)?.entryEdge == .bottom, "top exit enters bottom")
        try check(layout.remoteEdge(a, x: .max, y: 32768)?.target == b, "remote to remote: same screen ID on different devices")
        try check(layout.remoteEdge(c, x: .max, y: 32768)?.target == d, "adjacent displays on one receiver")
        try check(layout.remoteEdge(b, x: .max, y: 32768) == nil, "unconnected remote edge does not change focus")
        try check(layout.remoteEdge(a, x: 0, y: 32768)?.localEntryPoint.x == 991, "return to Mac is inset to avoid another crossing")
        try check(layout.remoteEdge(a, x: 1, y: 32000) == nil, "non-boundary LEAVE cannot change target")
        try check(layout.crossing(from: local, edge: .top, along: 1)?.target == d, "terminal offset remains inside the edge")
        let pair = try V2DesktopLayout.arrange([localScreen], [[first]], placements: [:])
        for side in V2LayoutEdge.allCases {
            guard let arranged = pair.placing([a], by: local, on: side), let seam = arranged.seams.first,
                  let entry = arranged.crossing(from: local, edge: side, along: 0.5) else {
                throw WireError.invalid("Missing seam for \(side)")
            }
            try check(seam.edge == side && entry.entryEdge == side.opposite && arranged.unreachable(using: arranged.seams).isEmpty,
                      "quick placement creates the requested crossing on \(side)")
            let x = side == .left ? UInt16.max : side == .right ? 0 : entry.x
            let y = side == .top ? UInt16.max : side == .bottom ? 0 : entry.y
            let back = arranged.remoteEdge(a, x: x, y: y)
            try check(back?.target == local && back?.entryEdge == side && localScreen.physical.contains(back!.localEntryPoint),
                      "unequal screens can return on \(side)")
            let reloaded = try V2DesktopLayout.arrange([localScreen], [[first]], placements: V2DesktopLayout.load(try arranged.encode(keeping: [:])) ?? [:])
            try check(reloaded.remoteEdge(a, x: x, y: y) == back, "saved \(side) route survives reload")
        }
        let below = try V2DesktopLayout.arrange([localScreen], [[first]], placements: [a: .init(x: 999, y: 800)])
        try check(below.remoteEdge(a, x: 0, y: 0)?.target == local, "a one-pixel overlap is reversible")
        try check(below.remoteEdge(a, x: 0, y: 0, accepts: { !$0.horizontalBoundary }) == nil, "corner cannot use an axis the pointer did not reach")
        try check(below.sourceEdge(.init(x: 999, y: 799), margin: 6, movement: .init(dx: 2, dy: 0)) == nil &&
                  below.sourceEdge(.init(x: 999, y: 799), margin: 6, movement: .init(dx: 0, dy: 2))?.target == a,
                  "only outward movement crosses the bottom seam")
        let corner = try V2DesktopLayout.arrange([localScreen], [[first]], placements: [a: .init(x: 1000, y: 800)])
        try check(corner.seams.isEmpty && corner.unreachable(using: corner.seams) == [a], "a corner touch has no traversable edge")
        let rightAbove = display(.init(device: "local", screen: 2), x: 1000, y: -900, w: 400, h: 400)
        let offsetDefault = try V2DesktopLayout.arrange([localScreen, rightAbove], [[first]], placements: [:])
        try check(offsetDefault.sourceEdge(.init(x: 1399, y: -800), margin: 6)?.target == a,
                  "default placement touches an offset rightmost Mac display")
        try check((try? V2DesktopLayout([localScreen, first])) == nil, "overlap rejected")
        try check((try? V2DesktopLayout([localScreen, localScreen])) == nil, "duplicate display rejected")
        try check(V2DesktopLayout.load(nil) == nil && V2DesktopLayout.load(Data(repeating: 0, count: 70_000)) == nil, "corrupt or oversized preference rejected")
        let offlineKey = V2DesktopLayout.Key(device: String(repeating: "E", count: 64), screen: 8)
        let encoded = try layout.encode(keeping: [offlineKey: .init(x: -640, y: 120)])
        let decoded = V2DesktopLayout.load(encoded)
        try check(decoded?[a] == placements[a] && decoded?[d] == placements[d] && decoded?[offlineKey]?.x == -640, "saved screen placement retains offline peers")
        print("PASS \(checks) macOS topology checks: four directions, partial edge overlaps, group screens and saved placements")
    }
}
