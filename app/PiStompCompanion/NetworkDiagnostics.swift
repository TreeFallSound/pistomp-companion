import AppKit
import Foundation
/// Collects a battery of unprivileged network/stack probes into a sectioned
/// log file, then opens it in Console.app. Every command gets a timeout and
/// a recorded exit status — a hung probe must not hang the dump.
enum NetworkDiagnostics {

    static let logDir = NSString(string: "~/Library/Logs/JackBridge").expandingTildeInPath

    static func run(shmSnapshot: ShmSnapshot, shmAttached: Bool, shmError: String?,
                    piWired: Bool, reachability: ReachabilityMonitor.Result) {
        let queue = DispatchQueue(label: "com.jackbridge.companion.diagnostics", qos: .userInitiated)
        queue.async {
            let text = collect(shmSnapshot: shmSnapshot, shmAttached: shmAttached,
                                shmError: shmError, piWired: piWired, reachability: reachability)
            writeAndOpen(text)
        }
    }

    // MARK: - collection

    private static func collect(shmSnapshot: Shm, shmAttached: Bool, shmError: String?,
                                piWired: Bool, reachability: ReachabilityMonitor.Result) -> String {
        var s = ""
        let stamp = ISO8601DateFormatter().string(from: Date())
        s += "JackBridge Network Diagnostics — \(stamp)\n"
        s += "pi reachable: \(reachability.reachable) (via \(reachability.via ?? "-"), addr \(reachability.resolvedAddress ?? "-"))\n"
        s += "JACK graph pi ports wired: \(piWired)\n\n"
        s += shmSection(snap: shmSnapshot, attached: shmAttached, error: shmError)
        s += commandSection()
        s += jackSection()
        s += sentinelSection()
        s += logTailSection()
        return s
    }

    private typealias Shm = ShmSnapshot

    private static func shmSection(snap: Shm, attached: Bool, error: String?) -> String {
        var s = "== shm snapshot ==\n"
        if let error {
            s += "attach failed: \(error)\n\n"
            return s
        }
        guard attached else {
            s += "not attached (stack down?)\n\n"
            return s
        }
        let rows: [(String, UInt64)] = [
            ("numberTimeStamps 0x100", snap.numberTimeStamps),
            ("zeroHostTime 0x108", snap.zeroHostTime),
            ("seed 0x110", snap.seed),
            ("syncMode 0x118", snap.syncMode),
            ("bufferSize 0x120", snap.bufferSize),
            ("driverStatus 0x128", snap.driverStatus),
            ("protocolVersion 0x130", snap.protocolVersion),
            ("daemonAlive 0x138", snap.daemonAlive),
            ("halAnchorSeq 0x140", snap.halAnchorSeq),
            ("halAnchorHostTime 0x148", snap.halAnchorHostTime),
            ("halAnchorSampleTime 0x150", snap.halAnchorSampleTime),
            ("halInputReadHead 0x158", snap.halInputReadHead),
            ("halOutputWriteHead 0x160", snap.halOutputWriteHead),
            ("halNFrames 0x168", snap.halNFrames),
            ("halSampleRate 0x170", snap.halSampleRate),
            ("readFrameNumber[0] 0x180", snap.readFrameNumber[0]),
            ("readFrameNumber[1] 0x190", snap.readFrameNumber[1]),
            ("writeFrameNumber[0] 0x188", snap.writeFrameNumber[0]),
            ("writeFrameNumber[1] 0x198", snap.writeFrameNumber[1]),
        ]
        for (name, v) in rows { s += String(format: "  %-26s %llu (0x%llx)\n", (name as NSString).utf8String!, v, v) }
        s += "\n"
        return s
    }

    private static func commandSection() -> String {
        var s = "== network state ==\n"
        let cached = ReachabilityMonitor.cachedIP ?? "-"
        s += "cached last-known-good IP: \(cached)\n"

        s += "\n-- SSID --\n"
        s += runCommand("/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport",
                        args: ["-I"], timeout: 5, includeExit: true)
        s += "\n-- networksetup -listallhardwareports --\n"
        s += runCommand("/usr/sbin/networksetup", args: ["-listallhardwareports"], timeout: 5, includeExit: true)
        s += "\n-- ifconfig --\n"
        s += runCommand("/sbin/ifconfig", args: [], timeout: 5, includeExit: true)
        s += "\n-- netstat -rn --\n"
        s += runCommand("/usr/sbin/netstat", args: ["-rn"], timeout: 5, includeExit: true)
        s += "\n-- route -n get default --\n"
        s += runCommand("/sbin/route", args: ["-n", "get", "default"], timeout: 5, includeExit: true)
        s += "\n-- arp -an --\n"
        s += runCommand("/usr/sbin/arp", args: ["-an"], timeout: 5, includeExit: true)

        s += "\n== name resolution ==\n"
        s += "\n-- dns-sd -Q pistomp.local (5s cap) --\n"
        s += runCommand("/usr/bin/dns-sd", args: ["-Q", "pistomp.local", "A"], timeout: 5, includeExit: false)
        s += "\n-- ping -c 3 -t 3 pistomp.local --\n"
        s += runCommand("/sbin/ping", args: ["-c", "3", "-t", "3", "pistomp.local"], timeout: 6, includeExit: true)

        s += "\n== TCP probes (3s cap) ==\n"
        for host in ["pistomp.local"] + (ReachabilityMonitor.cachedIP != nil ? [ReachabilityMonitor.cachedIP!] : []) {
            for port in [UInt16(22), 80] {
                s += "  \(host):\(port) → \(tcpProbeSync(host, port: port))\n"
            }
        }
        return s + "\n"
    }

    private static func jackSection() -> String {
        var s = "== JACK graph ==\n"
        let (out, st) = JackGraphMonitor.runJackLsp(connect: false)
        s += "-- jack_lsp (exit \(st)) --\n\(out)\n"
        let (cout, cst) = JackGraphMonitor.runJackLsp(connect: true)
        s += "-- jack_lsp -c (exit \(cst)) --\n\(cout)\n\n"
        return s
    }

    private static func sentinelSection() -> String {
        var s = "== route watcher sentinels ==\n"
        for path in ["/var/run/jackbridge-route.iface", "/var/run/jackbridge-ethernet.up"] {
            if let text = try? String(contentsOfFile: path, encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines) {
                s += "  \(path): \(text)\n"
            } else {
                s += "  \(path): (absent or unreadable)\n"
            }
        }
        return s + "\n"
    }

    private static func logTailSection() -> String {
        var s = "== /tmp log tails (last 50 lines each) ==\n"
        let names = ["com.jackbridge.jackd.err.log", "com.jackbridge.jackd.out.log",
                     "com.jackbridge.daemon.err.log", "com.jackbridge.daemon.out.log"]
        for n in names {
            let path = "/tmp/\(n)"
            s += "\n-- \(path) --\n"
            if let text = try? String(contentsOfFile: path, encoding: .utf8) {
                let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
                s += lines.suffix(50).joined(separator: "\n")
                s += "\n"
            } else {
                s += "  (absent)\n"
            }
        }
        return s + "\n"
    }

    // MARK: - process helpers

    static func runCommand(_ path: String, args: [String], timeout: TimeInterval,
                           includeExit: Bool) -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = args
        let out = Pipe(), err = Pipe()
        p.standardOutput = out; p.standardError = err
        do {
            try p.run()
        } catch {
            return "exec failed: \(error.localizedDescription)\n"
        }
        // Watchdog: SIGKILL after the budget. A hung probe must not hang the dump.
        let killer = DispatchWorkItem { if p.isRunning { p.interrupt() } }
        DispatchQueue.global().asyncAfter(deadline: .now() + timeout, execute: killer)
        p.waitUntilExit()
        killer.cancel()
        let o = String(data: out.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        let e = String(data: err.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        let exitLine = includeExit ? "[exit \(p.terminationStatus)]\n" : ""
        return exitLine + o + (e.isEmpty ? "" : e)
    }

    private static func writeAndOpen(_ text: String) {
        do {
            try FileManager.default.createDirectory(atPath: logDir, withIntermediateDirectories: true)
            let stamp = Date().jbStrftime("%Y%m%d-%H%M%S")
            let path = "\(logDir)/network-diagnostics-\(stamp).log"
            try text.write(toFile: path, atomically: true, encoding: .utf8)
            DispatchQueue.main.async {
                NSWorkspace.shared.open(URL(fileURLWithPath: path))
            }
        } catch {
            NSLog("JackBridge diagnostics write failed: \(error.localizedDescription)")
        }
    }
}

extension Date {
    func jbStrftime(_ fmt: String) -> String {
        var t = time_t(self.timeIntervalSince1970)
        var tmv = tm()
        localtime_r(&t, &tmv)
        var buf = [CChar](repeating: 0, count: 64)
        strftime(&buf, buf.count, fmt, &tmv)
        return String(cString: buf)
    }
}
