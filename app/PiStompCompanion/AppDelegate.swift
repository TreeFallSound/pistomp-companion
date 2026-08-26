import AppKit
import ServiceManagement
import Foundation

class AppDelegate: NSObject, NSApplicationDelegate {
    private let monitor = StatusMonitor()
    /// Last state published by the monitor. Main-queue only — the monitor's
    /// own copy is owned by its state queue and is never read from here.
    private var lastState = StatusMonitor.State()
    private var statusItem: StatusItemController!
    private var statusLineItem: NSMenuItem!
    private var moduiItem: NSMenuItem!
    private var sshItem: NSMenuItem!
    private var launchAtLoginItem: NSMenuItem!
    private var diagnosticsAlert: NSAlert?
    private static let piHost = "pistomp.local"
    private static let support = "/Library/Application Support/JackBridge"
    private static let ctl = "\(support)/jackbridge-ctl"

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = StatusItemController()
        statusItem.statusItem.menu = buildMenu()
        monitor.onUpdate = { [weak self] state in
            self?.render(state)
        }
        monitor.start()
    }

    // MARK: - rendering

    private func render(_ state: StatusMonitor.State) {
        let previousJackCondition = lastState.jackCondition
        lastState = state
        statusLineItem.title = state.detailLine
        moduiItem.action = state.piReachable ? #selector(openModUI(_:)) : nil
        sshItem.action = state.piReachable ? #selector(openSSH(_:)) : nil

        if state.jackCondition == .ourServer && previousJackCondition != .ourServer {
            confirmExistingJackServer()
        }

        let badge: StatusItemController.Badge
        switch state.health {
        case .protocolMismatch:
            badge = .red
        case .streaming:
            badge = .solidGreen
        case .startedIdle, .linkedIdle:
            badge = .hollowGreen
        case .piUnreachable:
            badge = state.piReachable ? .amber : .none
        case .stackDown:
            badge = .none
        }
        statusItem.update(badge: badge, reachable: state.piReachable)
    }

    private func confirmExistingJackServer() {
        let alert = NSAlert()
        alert.messageText = "JackBridge cannot start"
        alert.informativeText = "An existing JACK server belongs to JackBridge, but its launcher is no longer running. Quit that server and start JackBridge again?"
        alert.addButton(withTitle: "Quit Other Server")
        alert.addButton(withTitle: "Cancel")
        if alert.runModal() == .alertFirstButtonReturn {
            // Keep the stop/start sequence in jackbridge-ctl so this path and
            // Restart JackBridge share the same ownership guard.
            runCtl("restart")
        }
    }

    // MARK: - menu

    private func buildMenu() -> NSMenu {
        let m = NSMenu()

        statusLineItem = NSMenuItem(title: "…", action: nil, keyEquivalent: "")
        statusLineItem.isEnabled = false
        m.addItem(statusLineItem)
        m.addItem(.separator())

        m.addItem(item("Start JackBridge", #selector(startStack(_:))))
        m.addItem(item("Stop JackBridge", #selector(stopStack(_:))))
        m.addItem(item("Restart JackBridge", #selector(restartStack(_:))))
        m.addItem(.separator())

        moduiItem = item("Open MOD-UI", #selector(openModUI(_:)))
        sshItem = item("SSH to pi-Stomp", #selector(openSSH(_:)))
        m.addItem(moduiItem)
        m.addItem(sshItem)
        m.addItem(.separator())

        m.addItem(item("Network Diagnostics…", #selector(runDiagnostics(_:))))
        m.addItem(item("Open Logs", #selector(openLogs(_:))))
        m.addItem(item("Settings…", #selector(openSettings(_:))))

        launchAtLoginItem = item("Launch at Login", #selector(toggleLaunchAtLogin(_:)))
        refreshLaunchAtLogin()
        m.addItem(launchAtLoginItem)
        m.addItem(.separator())
        m.addItem(item("Quit", #selector(quit(_:)), key: "q"))
        return m
    }

    private func item(_ title: String, _ action: Selector, key: String = "") -> NSMenuItem {
        let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
        i.target = self
        return i
    }

    // MARK: - actions

    @objc private func startStack(_ s: Any?) { runCtl("start") }
    @objc private func stopStack(_ s: Any?) { runCtl("stop") }
    @objc private func restartStack(_ s: Any?) { runCtl("restart") }

    /// `jackbridge-ctl restart` boots two agents out and back in; it takes a
    /// few seconds on a good day and can wedge on a jackd that won't die, so
    /// it runs off-main and bounded rather than fire-and-forget.
    private func runCtl(_ sub: String) {
        DispatchQueue.global(qos: .userInitiated).async {
            let r = ProcessRunner.run(Self.ctl, args: [sub], timeout: 30)
            if let launchError = r.launchError {
                NSLog("jackbridge-ctl \(sub) failed to launch: \(launchError)")
            } else if r.timedOut {
                NSLog("jackbridge-ctl \(sub) timed out; killed")
            } else if r.status != 0 {
                NSLog("jackbridge-ctl \(sub) exit \(r.status.map(String.init) ?? "?"): \(r.combined)")
            }
        }
    }

    @objc private func openModUI(_ s: Any?) {
        var components = URLComponents()
        components.scheme = "http"
        components.host = Self.piHost
        components.path = "/"
        guard let url = components.url else {
            NSLog("Cannot open MOD-UI URL for %@", Self.piHost)
            return
        }
        if !NSWorkspace.shared.open(url) {
            NSLog("Cannot open MOD-UI URL %@", url.absoluteString)
        }
    }

    @objc private func openSSH(_ s: Any?) {
        // Always use the documented management hostname and explicitly launch
        // Terminal. An ssh:// URL is owned by whichever app registered that
        // scheme (often an IDE), and a transient endpoint may include an
        // interface scope that is not a valid URL host.
        let host = Self.piHost
        let script = """
        #!/bin/sh
        /usr/bin/ssh pistomp@\(host)
        status=$?
        /bin/rm -f -- "$0"
        exit $status
        """
        let path = FileManager.default.temporaryDirectory
            .appendingPathComponent("PiStompCompanion-ssh-\(UUID().uuidString).command")
        do {
            try script.write(to: path, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: path.path)
        } catch {
            NSLog("Cannot prepare SSH Terminal command: \(error.localizedDescription)")
            return
        }

        DispatchQueue.global(qos: .userInitiated).async {
            let result = ProcessRunner.run("/usr/bin/open", args: ["-a", "Terminal", path.path], timeout: 5)
            if result.launchError != nil || result.status != 0 {
                try? FileManager.default.removeItem(at: path)
                NSLog("Cannot open SSH in Terminal: \(result.combined)")
            }
        }
    }

    @objc private func runDiagnostics(_ s: Any?) {
        guard diagnosticsAlert == nil else { return }
        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.messageText = "Collecting network diagnostics…"
        alert.informativeText = "Checking the Pi, network routes, and JACK. This can take up to 30 seconds."
        alert.window.isReleasedWhenClosed = false
        diagnosticsAlert = alert
        NSApp.activate(ignoringOtherApps: true)
        alert.window.center()
        alert.window.orderFrontRegardless()
        NetworkDiagnostics.run(state: lastState) { [weak self] in
            self?.diagnosticsAlert?.window.close()
            self?.diagnosticsAlert = nil
        }
    }

    @objc private func openLogs(_ s: Any?) {
        for n in ["com.jackbridge.jackd.err.log", "com.jackbridge.jackd.out.log",
                  "com.jackbridge.daemon.err.log", "com.jackbridge.daemon.out.log"] {
            NSWorkspace.shared.open(URL(fileURLWithPath: "/tmp/\(n)"))
        }
    }

    /// Settings live in a root-owned plist. Phase 1: open it in the user's
    /// editor (zero privilege). Saving it restarts the agents via WatchPaths.
    @objc private func openSettings(_ s: Any?) {
        NSWorkspace.shared.open(URL(fileURLWithPath: "\(Self.support)/config.plist"))
    }

    @objc private func toggleLaunchAtLogin(_ s: Any?) {
        let svc = SMAppService.mainApp
        do {
            if launchAtLoginItem.state == .on {
                try svc.unregister()
            } else {
                try svc.register()
            }
        } catch {
            NSLog("SMAppService toggle failed: \(error.localizedDescription)")
        }
        refreshLaunchAtLogin()
    }

    private func refreshLaunchAtLogin() {
        launchAtLoginItem.state = SMAppService.mainApp.status == .enabled ? .on : .off
    }

    @objc private func quit(_ s: Any?) {
        NSApp.terminate(nil)
    }
}
