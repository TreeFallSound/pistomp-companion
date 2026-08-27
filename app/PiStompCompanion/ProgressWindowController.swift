import AppKit

/// Determinate progress window in the standard macOS style — like a file
/// copy or software update: leading icon, bold heading, bar, small status
/// line, no window controls. Starts indeterminate and flips determinate on
/// the first `advance`, so the window never opens on a motionless bar.
///
/// The window sizes itself from the laid-out content (`fittingSize`), which
/// is the only width/height authority — no hard-coded frame, so neither
/// padding nor text ever gets clipped or dead space.
final class ProgressWindowController: NSWindowController {

    private let bar = NSProgressIndicator()
    private let statusField = NSTextField(labelWithString: "Preparing…")

    init(title: String) {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1, height: 1),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.title = title
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.isMovableByWindowBackground = true
        window.isReleasedWhenClosed = false
        window.standardWindowButton(.closeButton)?.isHidden = true
        window.standardWindowButton(.miniaturizeButton)?.isHidden = true
        window.standardWindowButton(.zoomButton)?.isHidden = true
        super.init(window: window)

        // MenuBarIcon is a 64x64 asset (@2x 128) authored for the menu bar;
        // at 40pt it reads as a window icon without dominating.
        let icon = NSImageView(image: NSImage(named: "MenuBarIcon") ?? NSApp.applicationIconImage)
        icon.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            icon.widthAnchor.constraint(equalToConstant: 40),
            icon.heightAnchor.constraint(equalToConstant: 40),
        ])

        let heading = NSTextField(labelWithString: title)
        heading.font = .boldSystemFont(ofSize: NSFont.systemFontSize)
        heading.lineBreakMode = .byTruncatingTail

        // The status field is the widest content; give it compression
        // resistance so fittingSize tracks the worst real probe label.
        statusField.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        statusField.textColor = .secondaryLabelColor
        statusField.lineBreakMode = .byTruncatingTail
        statusField.setContentCompressionResistancePriority(.required, for: .horizontal)
        // Widest plausible label; replaced as probes land.
        statusField.stringValue = "com.treefallsound.companion.daemon.err.log (14 of 14)"

        bar.style = .bar
        bar.isIndeterminate = true
        bar.startAnimation(nil)
        bar.translatesAutoresizingMaskIntoConstraints = false

        let textColumn = NSStackView(views: [heading, bar, statusField])
        textColumn.orientation = .vertical
        textColumn.alignment = .leading
        textColumn.spacing = 8
        textColumn.translatesAutoresizingMaskIntoConstraints = false
        bar.leadingAnchor.constraint(equalTo: textColumn.leadingAnchor).isActive = true
        bar.trailingAnchor.constraint(equalTo: textColumn.trailingAnchor).isActive = true

        let row = NSStackView(views: [icon, textColumn])
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 14
        row.translatesAutoresizingMaskIntoConstraints = false

        let content = window.contentView!
        content.addSubview(row)
        NSLayoutConstraint.activate([
            row.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 20),
            row.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -20),
            row.topAnchor.constraint(equalTo: content.topAnchor, constant: 12),
            row.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -14),
        ])
        // Fit content exactly, then pin the size so a shorter later status
        // never shrinks the window mid-run.
        window.setContentSize(row.fittingSize)
        let min = window.frameRect(forContentRect: NSRect(origin: .zero, size: row.fittingSize)).size
        window.contentMinSize = min
        window.contentMaxSize = min
        window.center()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    /// Advances the bar and shows which probe just finished. Main queue only.
    func advance(completed: Int, total: Int, label: String) {
        if bar.isIndeterminate {
            bar.stopAnimation(nil)
            bar.isIndeterminate = false
            bar.minValue = 0
        }
        bar.maxValue = Double(total)
        bar.doubleValue = Double(completed)
        statusField.stringValue = "\(label) (\(completed) of \(total))"
    }
}
