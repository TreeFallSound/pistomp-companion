import AppKit
import ServiceManagement
import Foundation

class AppDelegate: NSObject, NSApplicationDelegate {
    private let monitor = StatusMonitor()
    private var statusItem: StatusItemController!
    private var statusLineItem: NSMenuItem!
    private var moduiItem: NSMenuItem!
    private var sshItem: NSMenuItem!
    private var launchAtLoginItem: NSMenuItem!

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
        statusLineItem.title = state.detailLine
        moduiItem.action = state.piReachable ? #selector(openModUI(_:)) : nil
        sshItem.action = state.piReachable ? #selector(openSSH(_:)) : nil

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

    private func runCtl(_ sub: String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: Self.ctl)
        p.arguments = [sub]
        p.standardOutput = Pipe(); p.standardError = Pipe()
        do { try p.run() } catch { NSLog("jackbridge-ctl \(sub) failed: \(error)") }
    }

    @objc private func openModUI(_ s: Any?) {
        guard let addr = monitor.state.resolvedAddress else { return }
        NSWorkspace.shared.open(URL(string: "http://\(addr)/")!)
    }

    @objc private func openSSH(_ s: Any?) {
        guard let addr = monitor.state.resolvedAddress else { return }
        // Goes to the user's registered ssh:// handler (Terminal by default)
        // with no TCC prompt. Resolved address, not the hostname.
        NSWorkspace.shared.open(URL(string: "ssh://pistomp@\(addr)")!)
    }

    @objc private func runDiagnostics(_ s: Any?) {
        let st = monitor.state
        NetworkDiagnostics.run(shmSnapshot: st.snapshot, shmAttached: st.shmAttached,
                               shmError: st.shmError, piWired: st.piWired,
                               reachability: ReachabilityMonitor.Result(
                                   reachable: st.piReachable,
                                   resolvedAddress: st.resolvedAddress,
                                   via: st.resolvedAddress != nil ? "probe" : nil))
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
