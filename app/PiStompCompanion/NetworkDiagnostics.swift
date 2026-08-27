import AppKit
import Foundation
/// Collects a battery of unprivileged network/stack probes into a sectioned
/// log file, then opens it in Console.app. Every command gets a timeout and
/// a recorded exit status — a hung probe must not hang the dump.
enum NetworkDiagnostics {

    static let logDir = NSString(string: "~/Library/Logs/JackBridge").expandingTildeInPath

    /// Runs off-main and calls completion on the main queue after the report
    /// has been written and opened. `onProgress` fires on the main queue as
    /// each probe lands: (completed, total, label of the probe that finished).
    static func run(state: StatusMonitor.State,
                    onProgress: @escaping (Int, Int, String) -> Void = { _, _, _ in },
                    completion: @escaping () -> Void = {}) {
        let queue = DispatchQueue(label: "com.treefallsound.companion.diagnostics", qos: .userInitiated)
        queue.async {
            writeAndOpen(collect(state: state, onProgress: onProgress), completion: completion)
        }
    }
    private static func writeAndOpen(_ text: String, completion: @escaping () -> Void) {
        do {
            try FileManager.default.createDirectory(atPath: logDir, withIntermediateDirectories: true)
            let stamp = Date().jbStrftime("%Y%m%d-%H%M%S")
            let path = "\(logDir)/network-diagnostics-\(stamp).log"
            try text.write(toFile: path, atomically: true, encoding: .utf8)
            DispatchQueue.main.async {
                NSWorkspace.shared.open(URL(fileURLWithPath: path))
                completion()
            }
        } catch {
            NSLog("JackBridge diagnostics write failed: \(error.localizedDescription)")
            DispatchQueue.main.async(execute: completion)
        }
    }

    // MARK: - collection

    private typealias Shm = ShmSnapshot

    /// One bounded probe plus its human-readable progress label. `id` keys
    /// the result dict, so reusing one probe's body for two report sections
    /// (only the TCP probe, per host) renders both.
    private struct Probe {
        let id: String
        let label: String
        let body: () -> String
    }

    private static func collect(state: StatusMonitor.State,
                                onProgress: @escaping (Int, Int, String) -> Void) -> String {
        var s = ""
        let stamp = ISO8601DateFormatter().string(from: Date())
        s += "JackBridge Network Diagnostics — \(stamp)\n"
        s += "status: \(state.detailLine)\n"
        s += "pi reachable: \(state.piReachable) (via \(state.reachedVia ?? "-"), addr \(state.resolvedAddress ?? "-"))\n"
        s += "JACK graph pi ports wired: \(state.piWired)"
        s += state.jackGraphError.map { " (\($0))" } ?? ""
        s += "\n"
        s += "JACK prefix: \(JackTools.prefix) (\(JackTools.prefixOrigin))\n\n"

        // The shm snapshot is already in `state` — zero work, report
        // synchronously.
        let shmText = shmSection(snap: state.snapshot, attached: state.shmAttached, error: state.shmError)

        let commandProbes = makeCommandProbes()
        let jackProbes: [Probe] = [
            Probe(id: "jack_lsp", label: "JACK port list") {
                JackGraphMonitor.runJackLsp(connect: false).report(includeExit: true)
            },
            Probe(id: "jack_lsp_c", label: "JACK connections") {
                JackGraphMonitor.runJackLsp(connect: true).report(includeExit: true)
            },
        ]
        let logTailProbes: [Probe] = ["com.treefallsound.companion.jackd.err.log", "com.treefallsound.companion.jackd.out.log",
                                      "com.treefallsound.companion.daemon.err.log", "com.treefallsound.companion.daemon.out.log"]
            .map { name in
                Probe(id: name, label: name) { logTail(path: "/tmp/\(name)") }
            }

        let all = commandProbes + jackProbes + logTailProbes
        var results = [String: String]()
        let resultsLock = NSLock()
        let doneLock = NSLock()
        var done = 0

        // Every probe is already individually bounded (ProcessRunner, the
        // tcpProbeSync semaphore); running them concurrently turns the dump's
        // wall time from the SUM of budgets into the LONGEST one, and the
        // group.wait(timeout:) deadline below just backstops that — a wedged probe
        // keeps costing nobody nothing.
        let group = DispatchGroup()
        for p in all {
            group.enter()
            DispatchQueue.global(qos: .userInitiated).async {
                let text = p.body()
                resultsLock.lock()
                results[p.id] = text
                resultsLock.unlock()
                doneLock.lock()
                done += 1
                let completed = done
                doneLock.unlock()
                DispatchQueue.main.async { onProgress(completed, all.count, p.label) }
                group.leave()
            }
        }
        _ = group.wait(timeout: .now() + 45)

        s += shmText
        s += commandSection(probes: commandProbes, results: results)
        s += jackSection(results: results)
        s += sentinelSection()
        s += logTailSection(probes: logTailProbes, results: results)
        return s
    }

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

    private static func runCommand(_ path: String, args: [String], timeout: TimeInterval,
                                   includeExit: Bool) -> String {
        ProcessRunner.run(path, args: args, timeout: timeout).report(includeExit: includeExit)
    }

    private static func makeCommandProbes() -> [Probe] {
        let hostname = JackTools.piHostname
        var probes: [Probe] = [
            Probe(id: "airport", label: "Wi-Fi interface") {
                runCommand("/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport",
                           args: ["-I"], timeout: 5, includeExit: true)
            },
            Probe(id: "networksetup", label: "Hardware ports") {
                runCommand("/usr/sbin/networksetup", args: ["-listallhardwareports"], timeout: 5, includeExit: true)
            },
            Probe(id: "ifconfig", label: "Interfaces") {
                runCommand("/sbin/ifconfig", args: [], timeout: 5, includeExit: true)
            },
            Probe(id: "netstat", label: "Routing table") {
                runCommand("/usr/sbin/netstat", args: ["-rn"], timeout: 5, includeExit: true)
            },
            Probe(id: "route", label: "Default route") {
                runCommand("/sbin/route", args: ["-n", "get", "default"], timeout: 5, includeExit: true)
            },
            Probe(id: "arp", label: "ARP table") {
                runCommand("/usr/sbin/arp", args: ["-an"], timeout: 5, includeExit: true)
            },
            Probe(id: "dns-sd", label: "Name resolution") {
                runCommand("/usr/bin/dns-sd", args: ["-Q", hostname, "A"], timeout: 5, includeExit: false)
            },
            Probe(id: "ping", label: "Ping \(hostname)") {
                runCommand("/sbin/ping", args: ["-c", "3", "-t", "3", hostname], timeout: 6, includeExit: true)
            },
        ]
        // The tcpProbeSync result is reused for every report section, so one
        // probe per host covers ports 22 and 80 in both the hostname and
        // last-known-good-IP lines below.
        for host in [hostname] + (ReachabilityMonitor.cachedIP != nil ? [ReachabilityMonitor.cachedIP!] : []) {
            probes.append(Probe(id: "tcp-\(host)", label: "TCP probe \(host)") {
                let line22 = tcpProbeSync(host, port: 22)
                let line80 = tcpProbeSync(host, port: 80)
                return "  \(host):22 → \(line22)\n  \(host):80 → \(line80)\n"
            })
        }
        return probes
    }

    private static func commandSection(probes: [Probe], results: [String: String]) -> String {
        var s = "== network state ==\n"
        let cached = ReachabilityMonitor.cachedIP ?? "-"
        s += "cached last-known-good IP: \(cached)\n"

        for p in probes where !p.id.hasPrefix("tcp-") {
            let heading: String
            switch p.id {
            case "airport":      heading = "SSID"
            case "networksetup": heading = "networksetup -listallhardwareports"
            case "dns-sd":       heading = "dns-sd -Q \(JackTools.piHostname) (5s cap)"
            case "ping":         heading = "ping -c 3 -t 3 \(JackTools.piHostname)"
            default:             heading = p.id
            }
            if p.id == "dns-sd" {
                s += "\n== name resolution ==\n"
            }
            s += "\n-- \(heading) --\n"
            s += results[p.id] ?? "[did not complete]\n"
        }

        s += "\n== TCP probes (3s cap) ==\n"
        for p in probes where p.id.hasPrefix("tcp-") {
            s += results[p.id] ?? "[did not complete]\n"
        }
        return s + "\n"
    }

    private static func jackSection(results: [String: String]) -> String {
        var s = "== JACK graph ==\n"
        s += "jack_lsp: \(JackTools.jackLsp)\n"
        s += "\n-- jack_lsp --\n"
        s += results["jack_lsp"] ?? "[did not complete]\n"
        s += "\n-- jack_lsp -c --\n"
        s += results["jack_lsp_c"] ?? "[did not complete]\n"
        return s + "\n"
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

    private static func logTail(path: String) -> String {
        if let text = try? String(contentsOfFile: path, encoding: .utf8) {
            let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
            return lines.suffix(50).joined(separator: "\n") + "\n"
        }
        return "  (absent)\n"
    }

    private static func logTailSection(probes: [Probe], results: [String: String]) -> String {
        var s = "== /tmp log tails (last 50 lines each) ==\n"
        for p in probes {
            s += "\n-- /tmp/\(p.id) --\n"
            s += results[p.id] ?? "[did not complete]\n"
        }
        return s + "\n"
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
