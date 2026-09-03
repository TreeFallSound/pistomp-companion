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
    private var deployMenu: NSMenu!
    private var deployItem: NSMenuItem!
    private var deployRefreshGeneration = 0
    /// True from the click on a PR until its confirmation is answered. The
    /// working-copy survey is an ssh round trip behind a modal progress
    /// window, and the menu is already closed by then — without this a user
    /// who reopens the menu can stack a second survey and a second alert
    /// behind the first.
    private var deploySurveyRunning = false

    /// Monotonic nonce for JB_OFF_RESYNC_REQUEST. The driver compares the
    /// value against the last one it saw and acts only on a change, and the
    /// value it saw survives us: the shm region and the loaded plug-in both
    /// outlive the app. An in-memory counter restarting at 0 on every launch
    /// therefore hands the driver a value it may already have latched, and a
    /// resync request would silently do nothing on the first use of a run.
    /// Derive it from the clock instead, which is monotonic across launches.
    ///
    /// Currently unused: Task C (docs/plan-replug-recovery.md) removed the
    /// menu item that wrote the nonce. Keep the machinery — ShmWriter
    /// .pokeResync, the driver's answer in SA_Device.cpp — for a future
    /// automatic re-anchor; the driver honours a *change* in the value, so
    /// mach_absolute_time() guarantees freshness whenever that caller lands.
    @available(*, deprecated, message: "reserved for future automatic re-anchor")
    private func nextResyncNonce() -> UInt64 { mach_absolute_time() }

    /// Whether we've already offered SSH key setup this launch. The probe
    /// fires on reachability state transitions, which can flap; the user
    /// sees the dialog once per app run, not on each flap.
    private var sshPromptShown = false
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
    /// Retries the pi-side start that `jackbridge-ctl start` was allowed to
    /// skip. Lazy so it can capture `runCtl` — every subprocess the Companion
    /// spawns goes through that one bounded path.
    private lazy var piHealer = PiSlaveHealer { [weak self] sub, done in
        guard let self else { return done(false) }
        self.runCtl(sub, completion: done)
    }

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
        // Before anything visual: a slave that is down while the stack is up
        // is a bug we can fix, not a status to draw. Cheap and idempotent —
        // the healer is a pure state machine until it decides to act.
        //
        // Not while the user's own start/stop/restart is in flight: `stop`
        // tears the slave down deliberately, and a heal racing it would ask
        // the pi to start the unit that `stop` is in the middle of stopping —
        // leaving a slave running against a Mac that has gone away. Pausing
        // costs nothing, because the state that follows the operation is the
        // one worth acting on anyway.
        if pendingStackOperation == nil {
            piHealer.note(state)
        }
        // Offer guided key install the first time we spot a pi-shaped ssh
        // refusal this launch. Key: the pi answers TCP (state.piReachable)
        // but BatchMode ssh is refused — that's exactly the "no authorized
        // key" signature, as opposed to "pi's off" (no TCP) or "key exists
        // but wrong" (stays refused after install; we don't loop).
        if !sshPromptShown && state.piReachable && state.health == .piUnreachable {
            sshPromptShown = true
            DispatchQueue.global(qos: .userInitiated).async {
                if SSHKeyInstaller.diagnose() == .keyRejected ||
                   SSHKeyInstaller.diagnose() == .noKeyOrNoOffer {
                    DispatchQueue.main.async {
                        _ = SSHKeyInstaller.offerInstall()
                    }
                }
            }
        }

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
        deployItem.isEnabled = state.piReachable

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
        case .noAudioFromPi:
            badge = .amber
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

    /// The deploy is destructive only when the pi has something to lose, and
    /// `PiWorkingCopy.survey` is what settles which case this is. A warning
    /// that fired on every deploy — the old behaviour — told the user nothing
    /// and trained them to click through the one deploy that mattered. So a
    /// surveyed-clean tree gets a plain confirmation in the informational
    /// style, and every other case gets a warning that says what is at stake.
    ///
    /// The two cases we could not survey warn *without* claiming to know the
    /// cost. Unsurveyed is never rendered as clean.
    private func confirmDeployment(of pullRequest: GitHubPullRequest,
                                   given outcome: PiWorkingCopy.Outcome) -> Bool {
        let alert = NSAlert()
        let proceedTitle: String
        var destructive = true

        switch outcome {
        case .surveyed(let survey) where survey.isClean:
            // Nothing to discard. This is still a confirmation, because the
            // deploy restarts mod-ala-pi-stomp and cuts audio — but it is not
            // a warning, and it must not carry a warning's styling.
            destructive = false
            alert.alertStyle = .informational
            alert.messageText = "Deploy PR #\(pullRequest.number) to pi-Stomp?"
            alert.informativeText = """
            \(Self.workingCopyPhrase(survey)) has no local changes, so nothing is discarded.

            \(Self.audioNotice)
            """
            proceedTitle = "Deploy"

        case .surveyed(let survey):
            let total = survey.trackedChangeCount + survey.removalCount
            alert.alertStyle = .warning
            alert.messageText = "Deploy PR #\(pullRequest.number) and discard "
                + "\(total) local \(total == 1 ? "change" : "changes") on the pi?"
            alert.informativeText = Self.lossDetail(survey)
            proceedTitle = "Deploy and Discard Changes"

        case .unexpanded:
            alert.alertStyle = .warning
            alert.messageText = "Deploy PR #\(pullRequest.number) to an unexpanded pi-Stomp tree?"
            alert.informativeText = """
            The pi's ~/pi-stomp carries no Git repository yet, so its local changes cannot be listed. \
            Deploying expands the repository, then deletes every untracked and ignored file and \
            discards tracked changes.

            Any edit made to the shipped source is lost, and it cannot be shown here first.

            \(Self.audioNotice)
            """
            proceedTitle = "Deploy and Discard Changes"

        case .unreadable(let reason):
            alert.alertStyle = .warning
            alert.messageText = "Deploy PR #\(pullRequest.number) without checking the pi?"
            alert.informativeText = """
            The pi's working copy could not be read: \(reason).

            Deploying still deletes every untracked and ignored file in ~/pi-stomp and discards \
            tracked changes. What that costs is unknown.

            \(Self.audioNotice)
            """
            proceedTitle = "Deploy and Discard Changes"
        }

        alert.addButton(withTitle: proceedTitle)
        alert.addButton(withTitle: "Cancel")
        if destructive {
            // Return must not fire an unrecoverable deploy. Hand the default
            // to Cancel; Escape already maps there by title.
            alert.buttons[0].keyEquivalent = ""
            alert.buttons[1].keyEquivalent = "\r"
        }
        NSApp.activate(ignoringOtherApps: true)
        return alert.runModal() == .alertFirstButtonReturn
    }

    private static let audioNotice =
        "Deploying checks out the pull request and restarts mod-ala-pi-stomp, which interrupts audio."

    /// "The pi's working copy on main at a1b2c3d", degrading through
    /// "detached at a1b2c3d" to the bare phrase as fields go unknown.
    private static func workingCopyPhrase(_ survey: PiWorkingCopy.Survey) -> String {
        var parts = ["The pi's working copy"]
        if let branch = survey.branch {
            parts.append("on \(branch)")
        } else if survey.head != nil {
            parts.append("detached")
        }
        if let head = survey.head { parts.append("at \(head)") }
        return parts.joined(separator: " ")
    }

    private static func lossDetail(_ survey: PiWorkingCopy.Survey) -> String {
        var blocks = ["\(Self.workingCopyPhrase(survey)) carries changes this deploy destroys."]
        if survey.trackedChangeCount > 0 {
            blocks.append(Self.section("Discarded — tracked edits",
                                       count: survey.trackedChangeCount,
                                       shown: survey.trackedChanges))
        }
        if survey.removalCount > 0 {
            blocks.append(Self.section("Deleted — untracked and ignored",
                                       count: survey.removalCount,
                                       shown: survey.removals))
        }
        blocks.append(Self.audioNotice)
        return blocks.joined(separator: "\n\n")
    }

    /// `count` is the pi's exact total; `shown` is already capped twice — once
    /// for transport, once here. Never report `shown.count` as the total.
    private static func section(_ heading: String, count: Int, shown: [String]) -> String {
        let visible = shown.prefix(8)
        var lines = ["\(heading) (\(count)):"]
        lines.append(contentsOf: visible.map { "    \($0)" })
        let remainder = count - visible.count
        if remainder > 0 { lines.append("    …and \(remainder) more") }
        return lines.joined(separator: "\n")
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
        deployMenu = NSMenu()
        deployMenu.delegate = self
        deployItem = NSMenuItem(title: "Deploy", action: nil, keyEquivalent: "")
        deployItem.submenu = deployMenu
        m.addItem(moduiItem)
        m.addItem(sshItem)
        m.addItem(deployItem)
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

extension AppDelegate: NSMenuDelegate {
    func menuWillOpen(_ menu: NSMenu) {
        guard menu === deployMenu else { return }
        refreshDeployMenu()
    }

    private func refreshDeployMenu() {
        deployRefreshGeneration += 1
        let generation = deployRefreshGeneration
        deployMenu.removeAllItems()
        let loading = NSMenuItem(title: "Loading…", action: nil, keyEquivalent: "")
        loading.isEnabled = false
        deployMenu.addItem(loading)

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let result = GitHubPullRequests.loadOpen()
            DispatchQueue.main.async {
                guard let self, generation == self.deployRefreshGeneration else { return }
                self.showDeployMenu(result)
            }
        }
    }

    private func showDeployMenu(_ result: Result<[GitHubPullRequest], GitHubPullRequests.LoadError>) {
        deployMenu.removeAllItems()
        switch result {
        case .success(let pullRequests) where pullRequests.isEmpty:
            let empty = NSMenuItem(title: "No open pi-Stomp PRs", action: nil, keyEquivalent: "")
            empty.isEnabled = false
            deployMenu.addItem(empty)
        case .success(let pullRequests):
            for pullRequest in pullRequests {
                let entry = item(pullRequest.menuTitle, #selector(deployPullRequest(_:)))
                entry.representedObject = pullRequest
                deployMenu.addItem(entry)
            }
        case .failure(let error):
            NSLog("Cannot list pi-Stomp pull requests: %@", error.description)
            let title: String
            switch error {
            case .unavailable:
                title = "GitHub CLI not installed"
            case .authenticationFailed:
                title = "GitHub CLI authentication failed"
            case .commandFailed, .invalidOutput:
                title = "Unable to load pull requests"
            }
            let failure = NSMenuItem(title: title, action: nil, keyEquivalent: "")
            failure.isEnabled = false
            deployMenu.addItem(failure)
        }
    }

    @objc private func deployPullRequest(_ sender: NSMenuItem) {
        guard let pullRequest = sender.representedObject as? GitHubPullRequest,
              !deploySurveyRunning else { return }
        deploySurveyRunning = true

        // The survey is an ssh round trip with a 20 s ceiling. A click that
        // does nothing visible for several seconds reads as a click that
        // missed, so the wait gets the same indeterminate window the
        // diagnostics collector uses.
        let progress = ProgressWindowController(title: "Checking the pi-Stomp working copy…")
        NSApp.activate(ignoringOtherApps: true)
        progress.window?.orderFrontRegardless()

        DispatchQueue.global(qos: .userInitiated).async {
            let outcome = PiWorkingCopy.survey()
            DispatchQueue.main.async {
                progress.window?.close()
                self.deploySurveyRunning = false
                guard self.confirmDeployment(of: pullRequest, given: outcome) else { return }
                self.openDeploymentTerminal(for: pullRequest, replacing: outcome)
            }
        }
    }
    /// `replacing` is the survey that justified the confirmation. It is used
    /// for one line of output: the commit the deploy is about to detach away
    /// from. The deploy leaves a detached HEAD, so without printing it here
    /// the way back to the tree the user had is gone from every record.
    private func openDeploymentTerminal(for pullRequest: GitHubPullRequest,
                                        replacing outcome: PiWorkingCopy.Outcome) {
        let target = Self.shellQuote("pistomp@\(JackTools.piHostname)")
        let restoreEcho: String
        if case .surveyed(let survey) = outcome, let ref = survey.branch ?? survey.head {
            restoreEcho = "echo " + Self.shellQuote(
                "Replacing \(ref). To return: cd ~/pi-stomp && git checkout \(ref)")
        } else {
            // No `:` no-op here — the line is simply absent from the script.
            restoreEcho = ""
        }
        let script = """
        #!/bin/sh
        set -u
        TARGET=\(target)
        PR_NUMBER=\(pullRequest.number)

        echo "Deploying pi-Stomp PR #$PR_NUMBER to $TARGET"
        \(restoreEcho)
        echo
        ssh -tt "$TARGET" /bin/sh -s -- "$PR_NUMBER" <<'REMOTE_SCRIPT'
        set -eu
        PR_NUMBER="$1"
        SRC="$HOME/pi-stomp"

        if [ ! -d "$SRC/.git" ]; then
            echo "==> Expanding pi-Stomp Git repository"
            "$SRC/util/expand-git.sh"
        fi

        cd "$SRC"
        echo "==> Removing local pi-Stomp source changes"
        git clean -fdx -e \(PiWorkingCopy.cleanExclusion)
        echo "==> Fetching refs/pull/$PR_NUMBER/head"
        git fetch --force origin "refs/pull/$PR_NUMBER/head"
        echo "==> Checking out PR head"
        git checkout --detach --force FETCH_HEAD
        echo "==> Restarting mod-ala-pi-stomp"
        sudo systemctl restart mod-ala-pi-stomp
        if ! sudo systemctl is-active --quiet mod-ala-pi-stomp; then
            echo "ERROR: mod-ala-pi-stomp failed to start." >&2
            sudo systemctl status --no-pager mod-ala-pi-stomp || true
            exit 1
        fi
        echo "==> pi-Stomp PR #$PR_NUMBER is deployed."
        REMOTE_SCRIPT

        status=$?
        echo
        if [ "$status" -eq 0 ]; then
            echo "Deployment complete."
        else
            echo "Deployment failed (exit $status)." >&2
        fi
        /bin/rm -f -- "$0"
        exit "$status"
        """
        let path = FileManager.default.temporaryDirectory
            .appendingPathComponent("PiStompCompanion-deploy-\(pullRequest.number)-\(UUID().uuidString).command")
        do {
            try script.write(to: path, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: path.path)
        } catch {
            NSLog("Cannot prepare deployment Terminal command: %@", error.localizedDescription)
            return
        }

        DispatchQueue.global(qos: .userInitiated).async {
            let result = ProcessRunner.run("/usr/bin/open", args: ["-a", "Terminal", path.path], timeout: 5)
            if result.launchError != nil || result.status != 0 {
                try? FileManager.default.removeItem(at: path)
                NSLog("Cannot open deployment in Terminal: %@", result.combined)
            }
        }
    }

    private static func shellQuote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}
