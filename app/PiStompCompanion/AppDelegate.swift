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
    private var startItem: NSMenuItem!
    private var stopItem: NSMenuItem!
    private var restartItem: NSMenuItem!
    private var moduiItem: NSMenuItem!
    private var sshItem: NSMenuItem!
    private var launchAtLoginItem: NSMenuItem!
    private var diagnosticsProgress: ProgressWindowController?
    private var settingsWindow: SettingsWindowController?
    private enum StackControlOperation: Equatable {
        case starting
        case stopping
        case restarting
    }
    private var pendingStackOperation: StackControlOperation?
    private var controlCommandFinished = false
    private var terminationPending = false
    private static let support = "/Library/Application Support/JackBridge"
    private static let ctl = "\(support)/jackbridge-ctl"

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = StatusItemController()
        statusItem.statusItem.menu = buildMenu()
        monitor.onUpdate = { [weak self] state in
            self?.render(state)
        }
        monitor.start()
        // Launching the app deliberately does NOT start the stack: jackd and
        // the daemon hold a realtime thread, and the app is a login item, so
        // an auto-start would put that cost on every boot without the user
        // asking. "Start JackBridge" in the menu is the only way up. Quitting
        // still tears the stack down — the app owns what it started.
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard !terminationPending else { return .terminateLater }
        terminationPending = true
        monitor.stop()
        runCtl("stop") { _ in
            NSApp.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }

    // MARK: - rendering

    private func render(_ state: StatusMonitor.State) {
        let previousJackCondition = lastState.jackCondition
        lastState = state
        if let operation = pendingStackOperation, controlCommandFinished {
            switch operation {
            case .starting where state.health != .stackDown:
                finishStackOperation()
            case .stopping where state.health == .stackDown:
                finishStackOperation()
            default:
                break
            }
        }
        updateStackControls()
        // An in-flight control command outranks the monitor's reading. The
        // stack spends the first seconds of a start indistinguishable from a
        // stopped one — no region, no heartbeat — so `detailLine` would sit
        // on "JackBridge stack down" through the whole launch and read as a
        // failure. `pendingStackOperation` is cleared above the moment the
        // health actually moves, so this never outlives the operation.
        statusLineItem.title = pendingStackOperation.map(Self.progressLine(for:)) ?? state.detailLine
        moduiItem.action = state.piReachable ? #selector(openModUI(_:)) : nil
        sshItem.action = state.piReachable ? #selector(openSSH(_:)) : nil

        if state.jackCondition == .ourServer && previousJackCondition != .ourServer {
            confirmExistingJackServer()
        }

        // Brightness answers exactly one question: is JackBridge running and
        // costing this Mac anything? Every health except `stackDown` means
        // jackd and the daemon are up and holding a realtime thread, so the
        // icon is bright — whether or not the pi ever shows up. Where the pi
        // stands is the badge's job, and the two must not be conflated:
        // reachability is a wifi TCP probe that answers independently of
        // whether the local stack is running at all, so feeding it into
        // brightness made a running stack look off whenever the pi was away.
        let live = state.health != .stackDown

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
        statusItem.update(badge: badge, live: live)
    }

    private static func progressLine(for operation: StackControlOperation) -> String {
        switch operation {
        case .starting:   return "Starting JackBridge…"
        case .stopping:   return "Stopping JackBridge…"
        case .restarting: return "Restarting JackBridge…"
        }
    }

    private func updateStackControls() {
        guard pendingStackOperation == nil else {
            startItem.isEnabled = false
            stopItem.isEnabled = false
            restartItem.isEnabled = false
            return
        }
        let stackStarted = lastState.health != .stackDown
        startItem.isEnabled = !stackStarted
        stopItem.isEnabled = stackStarted
        restartItem.isEnabled = stackStarted
    }

    private func finishStackOperation() {
        pendingStackOperation = nil
        controlCommandFinished = false
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

        startItem = item("Start JackBridge", #selector(startStack(_:)))
        stopItem = item("Stop JackBridge", #selector(stopStack(_:)))
        stopItem.isEnabled = false
        restartItem = item("Restart JackBridge", #selector(restartStack(_:)))
        restartItem.isEnabled = false
        m.addItem(startItem)
        m.addItem(stopItem)
        m.addItem(restartItem)
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

    @objc private func startStack(_ s: Any?) {
        beginStackOperation(.starting, command: "start")
    }

    @objc private func stopStack(_ s: Any?) {
        beginStackOperation(.stopping, command: "stop")
    }

    @objc private func restartStack(_ s: Any?) {
        beginStackOperation(.restarting, command: "restart")
    }

    private func beginStackOperation(_ operation: StackControlOperation, command: String) {
        guard pendingStackOperation == nil else { return }
        pendingStackOperation = operation
        controlCommandFinished = false
        updateStackControls()

        // Do not leave the menu locked forever if launchd accepts the command
        // but the service never reaches a visible state.
        DispatchQueue.main.asyncAfter(deadline: .now() + 35) { [weak self] in
            guard let self, self.pendingStackOperation == operation else { return }
            self.finishStackOperation()
            self.updateStackControls()
        }

        runCtl(command) { [weak self] succeeded in
            guard let self, self.pendingStackOperation == operation else { return }
            if !succeeded {
                self.finishStackOperation()
                self.updateStackControls()
            } else if operation == .restarting {
                self.finishStackOperation()
                self.updateStackControls()
            } else {
                self.controlCommandFinished = true
                self.render(self.lastState)
            }
        }
    }

    /// `jackbridge-ctl restart` boots two agents out and back in; it takes a
    /// few seconds on a good day and can wedge on a jackd that won't die, so
    /// it runs off-main and bounded rather than fire-and-forget.
    private func runCtl(_ sub: String, completion: @escaping (Bool) -> Void = { _ in }) {
        DispatchQueue.global(qos: .userInitiated).async {
            let r = ProcessRunner.run(Self.ctl, args: [sub], timeout: 30)
            let succeeded = r.launchError == nil && !r.timedOut && r.status == 0
            if let launchError = r.launchError {
                NSLog("jackbridge-ctl \(sub) failed to launch: \(launchError)")
            } else if r.timedOut {
                NSLog("jackbridge-ctl \(sub) timed out; killed")
            } else if r.status != 0 {
                NSLog("jackbridge-ctl \(sub) exit \(r.status.map(String.init) ?? "?"): \(r.combined)")
            }
            DispatchQueue.main.async {
                completion(succeeded)
            }
        }
    }

    @objc private func openModUI(_ s: Any?) {
        var components = URLComponents()
        components.scheme = "http"
        let host = JackTools.piHostname
        components.host = host.contains(":") && !host.hasPrefix("[") ? "[\(host)]" : host
        components.path = "/"
        guard let url = components.url else {
            NSLog("Cannot open MOD-UI URL for %@", JackTools.piHostname)
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
        let host = JackTools.piHostname
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
        guard diagnosticsProgress == nil else { return }
        let progress = ProgressWindowController(title: "Collecting network diagnostics…")
        diagnosticsProgress = progress
        NSApp.activate(ignoringOtherApps: true)
        progress.window?.orderFrontRegardless()
        NetworkDiagnostics.run(state: lastState,
            onProgress: { completed, total, label in
                progress.advance(completed: completed, total: total, label: label)
            },
            completion: { [weak progress] in
                progress?.window?.close()
                self.diagnosticsProgress = nil
            })
    }

    @objc private func openLogs(_ s: Any?) {
        for n in ["com.treefallsound.companion.jackd.err.log", "com.treefallsound.companion.jackd.out.log",
                  "com.treefallsound.companion.daemon.err.log", "com.treefallsound.companion.daemon.out.log"] {
            NSWorkspace.shared.open(URL(fileURLWithPath: "/tmp/\(n)"))
        }
    }

    @objc private func openSettings(_ s: Any?) {
        if settingsWindow == nil { settingsWindow = SettingsWindowController() }
        settingsWindow?.present()
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
