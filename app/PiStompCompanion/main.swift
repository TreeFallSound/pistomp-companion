import AppKit

// No storyboard/nib in this target: @main on NSApplicationDelegate would
// synthesize NSApplicationMain, which only wires the delegate from a
// storyboard — leaving a headless, do-nothing event loop. Bootstrap
// explicitly instead.
let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.run()
