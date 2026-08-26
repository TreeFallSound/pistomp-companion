import AppKit
import Foundation

/// Menu-bar status item: template icon at 0.35/1.0 alpha, badge dot
/// composited lower-right. Once a colored badge is composited the image is
/// NOT a template, so badge colors must read on light and dark menu bars —
/// the system colors below adapt to appearance.
final class StatusItemController {
    let statusItem: NSStatusItem

    enum Badge {
        case none          // pi unreachable (dim)
        case amber         // reachable, no from_slave ports
        case hollowGreen   // ports wired, driverStatus != STARTED
        case solidGreen    // ports wired + HAL read head advancing
        case red           // protocol mismatch / daemon failing
    }

    init() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        statusItem.button?.image = Self.render(badge: .none, reachable: false)
    }

    func update(badge: Badge, reachable: Bool) {
        statusItem.button?.image = Self.render(badge: badge, reachable: reachable)
    }

    static func render(badge: Badge, reachable: Bool) -> NSImage {
        guard let base = NSImage(named: "MenuBarIcon")?.copy() as? NSImage else {
            return NSImage()
        }
        let side = 18.0
        if badge == .none {
            // Pure template path: let AppKit tint. Dim by re-drawing the
            // template at reduced alpha — NSImage has no alpha property.
            let out = NSImage(size: NSSize(width: side, height: side))
            out.lockFocus()
            NSGraphicsContext.current?.cgContext.setAlpha(reachable ? 1.0 : 0.35)
            base.draw(in: NSRect(x: 0, y: 0, width: side, height: side))
            out.unlockFocus()
            out.isTemplate = true
            return out
        }
        let out = NSImage(size: NSSize(width: side, height: side))
        out.lockFocus()
        let full = NSRect(x: 0, y: 0, width: side, height: side)
        NSGraphicsContext.current?.cgContext.setAlpha(reachable ? 1.0 : 0.35)
        base.draw(in: full)
        NSGraphicsContext.current?.cgContext.setAlpha(1.0)

        // Badge dot, lower-right, ~5 pt diameter with a hairline rim so it
        // reads against the (dark or light) bar behind it.
        let d = 5.0
        let inset = 1.0
        let rect = NSRect(x: side - d - inset, y: inset, width: d, height: d)
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
        out.unlockFocus()
        out.isTemplate = false
        return out
    }

    private static func drawDot(_ rect: NSRect, fill: NSColor?, stroke: NSColor? = nil) {
        if let fill {
            fill.setFill()
            rect.insetBy(dx: -0.5, dy: -0.5).fill()
        }
        if let stroke {
            stroke.setStroke()
            let path = NSBezierPath(ovalIn: rect.insetBy(dx: 0.5, dy: 0.5))
            path.lineWidth = 1.2
            path.stroke()
        }
    }
}
