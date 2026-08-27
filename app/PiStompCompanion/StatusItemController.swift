import AppKit
import Foundation

/// Menu-bar status item: template icon at 0.35/1.0 alpha, badge dot drawn
/// lower-right. `live` — not raw reachability — drives the dim: once the
/// stack is wired the pi is demonstrably there, whether or not the mDNS/TCP
/// probe (which rides wifi, not the audio link) answered this cycle.
///
/// The badge is a sibling *view* on the status button rather than pixels
/// composited into the image, because a colored badge would force
/// `isTemplate = false` and AppKit would then stop tinting the icon — leaving
/// it a black stroke on a dark menu bar, and un-inverted under the open-menu
/// highlight. Keeping the image a pure template leaves all of that to AppKit;
/// `BadgeView` draws in its own `draw(_:)`, so its system colors resolve
/// against the live effective appearance and refresh on a dark/light flip
/// with no work from us.
final class StatusItemController {
    let statusItem: NSStatusItem

    enum Badge {
        case none          // pi unreachable (dim)
        case amber         // reachable, no from_slave ports
        case hollowGreen   // ports wired, driverStatus != STARTED
        case solidGreen    // ports wired + HAL read head advancing
        case red           // protocol mismatch / daemon failing
    }

    /// Point size the template icon is drawn at, inside the (wider) button.
    fileprivate static let iconSide = 18.0

    private let badgeView = BadgeView()

    init() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = statusItem.button {
            badgeView.frame = button.bounds
            badgeView.autoresizingMask = [.width, .height]
            button.addSubview(badgeView)
        }
        update(badge: .none, live: false)
    }

    func update(badge: Badge, live: Bool) {
        statusItem.button?.image = Self.icon(live: live)
        badgeView.badge = badge
    }

    /// The base artwork, dimmed when not live. Stays a template so AppKit
    /// owns its color in every appearance and every button state.
    static func icon(live: Bool) -> NSImage {
        guard let base = NSImage(named: "MenuBarIcon")?.copy() as? NSImage else {
            return NSImage()
        }
        // The artwork's natural size is its 1x pixel size (64 pt). The status
        // button does not scale an oversized image — it clips it — so every
        // path out of here has to stamp the point size down to `iconSide`.
        guard !live else {
            base.size = NSSize(width: iconSide, height: iconSide)
            base.isTemplate = true
            return base
        }
        // Dim by re-drawing at reduced alpha — NSImage has no alpha property.
        let out = NSImage(size: NSSize(width: iconSide, height: iconSide))
        out.lockFocus()
        NSGraphicsContext.current?.cgContext.setAlpha(0.35)
        base.draw(in: NSRect(x: 0, y: 0, width: iconSide, height: iconSide))
        out.unlockFocus()
        out.isTemplate = true
        return out
    }
}

/// Transparent overlay on the status button that paints just the badge dot.
private final class BadgeView: NSView {
    var badge: StatusItemController.Badge = .none {
        didSet { needsDisplay = true }
    }

    override var isOpaque: Bool { false }

    /// Clicks belong to the button underneath.
    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    override func draw(_ dirtyRect: NSRect) {
        guard badge != .none else { return }

        // Anchor to the icon, not the button: the button is wider than the
        // 18 pt artwork, which sits centered in it.
        let side = StatusItemController.iconSide
        let icon = NSRect(x: (bounds.width - side) / 2,
                          y: (bounds.height - side) / 2,
                          width: side, height: side)

        // ~5 pt diameter, lower-right, with a hairline rim so it reads
        // against the (dark or light) bar behind it.
        let d = 5.0
        let inset = 1.0
        let rect = NSRect(x: icon.maxX - d - inset, y: icon.minY + inset,
                          width: d, height: d)
        switch badge {
        case .none:
            break
        case .amber:
            drawDot(rect, fill: .systemYellow)
        case .hollowGreen:
            drawDot(rect, fill: nil, stroke: .systemGreen)
        case .solidGreen:
            drawDot(rect, fill: .systemGreen)
        case .red:
            drawDot(rect, fill: .systemRed)
        }
    }

    private func drawDot(_ rect: NSRect, fill: NSColor?, stroke: NSColor? = nil) {
        if let fill {
            fill.setFill()
            NSBezierPath(ovalIn: rect).fill()
        }
        if let stroke {
            stroke.setStroke()
            let path = NSBezierPath(ovalIn: rect.insetBy(dx: 0.5, dy: 0.5))
            path.lineWidth = 1.2
            path.stroke()
        }
    }
}
