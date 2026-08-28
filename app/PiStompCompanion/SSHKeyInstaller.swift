import AppKit
import Foundation

/// Guided first-run SSH key install for the pi-Stomp.
///
/// Every ssh path this app owns — jackbridge-coordinator:91,
/// SettingsWindowController.probe(:375), AppDelegate.openSSH — uses
/// BatchMode=yes, so a Mac with no authorized key on the pi fails them all
/// identically. This helper detects that specific failure (vs. "pi
/// unreachable" / "key exists but rejected") and walks the user through the
/// one interactive step: typing the pi password *once* so ssh-copy-id can
/// put this Mac's key on the pi. After that, BatchMode works forever.
///
/// The password never touches this process: we hand the interactive session
/// to a real Terminal (pattern borrowed from AppDelegate.openSSH), so
/// ssh-copy-id's prompt goes to a tty the user sees, not a pipe we'd
/// accidentally log.
enum SSHKeyInstaller {

    enum Unreachability {
        /// ssh with BatchMode succeeded (or host doesn't need a key yet) —
        /// nothing to install.
        case ok
        /// BatchMode rejected auth → user has a key locally but the pi
        /// doesn't trust it (Permission denied (publickey)).
        case keyRejected
        /// No local identity or host never offered publickey.
        case noKeyOrNoOffer
        /// Couldn't reach the host at all — a key won't fix this; the user
        /// wants the cable/wifi checked first.
        case unreachable
    }

    static func diagnose() -> Unreachability {
        let ssh = ProcessInfo.processInfo.environment["JACKBRIDGE_SSH_PATH"] ?? "/usr/bin/ssh"
        let result = ProcessRunner.run(ssh, args: [
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=5",
            "-o", "ConnectionAttempts=1",
            "-o", "PreferredAuthentications=publickey",
            "pistomp@\(JackTools.piHostname)",
            "true",
        ], env: JackTools.environment, timeout: 8)
        guard let status = result.status else {
            return .unreachable
        }
        if status == 0 { return .ok }
        let out = result.combined.lowercased()
        if out.contains("permission denied") && out.contains("publickey") {
            return .keyRejected
        }
        if out.contains("no route to host") || out.contains("could not resolve")
            || out.contains("operation timed out") || out.contains("connection refused")
            || result.timedOut {
            return .unreachable
        }
        return .noKeyOrNoOffer
    }

    /// Present the guided install. Returns true if we kicked off a Terminal
    /// that will complete the install; false if the user declined or the
    /// failure isn't key-related (in which case the caller should fall
    /// through to the generic "check the pi" UI).
    static func offerInstall() -> Bool {
        guard ensureLocalKey() else { return false }

        let host = JackTools.piHostname
        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.messageText = "Install an SSH key on pi-Stomp?"
        alert.informativeText = """
            JackBridge talks to \(host) over ssh for start/stop/status. This Mac doesn't have a key \
            authorized on the pi yet, so those connections are refused.

            This will open Terminal and ask for the pi-Stomp password exactly once, to install this \
            Mac's public key. (Default Pi OS password is raspberry unless changed.)
            """
        alert.addButton(withTitle: "Install Key")
        alert.addButton(withTitle: "Not Now")
        guard alert.runModal() == .alertFirstButtonReturn else { return false }

        let script = """
        #!/bin/sh
        set -e
        echo "Installing public key on pistomp@\(host)…"
        echo "When prompted, enter the pi-Stomp password (default: raspberry)."
        echo
        /usr/bin/ssh-copy-id -o ConnectTimeout=8 -o ConnectionAttempts=1 pistomp@\(host)
        echo
        echo "Key installed. You can close this window."
        sleep 2
        /bin/rm -f -- "$0"
        """
        let path = FileManager.default.temporaryDirectory
            .appendingPathComponent("PiStompCompanion-keyinstall-\(UUID().uuidString).command")
        do {
            try script.write(to: path, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: path.path)
        } catch {
            NSLog("Cannot prepare ssh-copy-id command: \(error.localizedDescription)")
            return false
        }

        DispatchQueue.global(qos: .userInitiated).async {
            let result = ProcessRunner.run("/usr/bin/open", args: ["-a", "Terminal", path.path], timeout: 5)
            if result.launchError != nil || result.status != 0 {
                try? FileManager.default.removeItem(at: path)
                NSLog("Cannot open Terminal for ssh-copy-id: \(result.combined)")
            }
        }
        return true
    }

    /// Make sure there's a local identity at ~/.ssh/id_ed25519 (generate one
    /// with an empty passphrase if absent). ssh-copy-id can't do its job
    /// until some key exists, and asking the user about key types is not a
    /// musician question — ed25519 is the correct boring answer.
    @discardableResult
    private static func ensureLocalKey() -> Bool {
        let home = FileManager.default.homeDirectoryForCurrentUser
        let keyPath = home.appendingPathComponent(".ssh/id_ed25519").path
        if FileManager.default.fileExists(atPath: keyPath) { return true }

        let sshDir = home.appendingPathComponent(".ssh").path
        do {
            try FileManager.default.createDirectory(atPath: sshDir,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700])
        } catch {
            NSLog("Cannot create ~/.ssh: \(error.localizedDescription)")
            return false
        }
        let result = ProcessRunner.run("/usr/bin/ssh-keygen", args: [
            "-t", "ed25519",
            "-f", keyPath,
            "-N", "",
            "-C", "pistomp-companion@\(Host.current().localizedName ?? "mac")",
        ], timeout: 15)
        if result.launchError != nil || result.status != 0 {
            NSLog("ssh-keygen failed: \(result.combined)")
            return false
        }
        return true
    }
}
