import Foundation

/// Polls `jack_lsp` for the pi's netJACK2 slave ports.
///
/// Ports live in JACK's graph, not in shm, so this shells out at 0.5 Hz.
/// `JACK_NO_START_SERVER=1` is mandatory: without it jack_lsp auto-spawns a
/// stray default jackd and can trip a TCC microphone prompt (same reason
/// `jackbridge/installer/jackd-launch` exports it).
final class JackGraphMonitor {
    /// True when the JACK graph contains `from_slave` / `pistomp` ports.
    private(set) var piWired = false
    private(set) var lastError: String?

    private let queue = DispatchQueue(label: "com.jackbridge.companion.jackgraph", qos: .utility)
    private var timer: DispatchSourceTimer?

    var onUpdate: (() -> Void)?

    func start(interval: TimeInterval = 2.0) {
        stop()
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: interval)
        t.setEventHandler { [weak self] in self?.poll() }
        timer = t
        t.resume()
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    /// Runs jack_lsp synchronously on the caller's queue. 5 s wall clock —
    /// if jackd's socket is wedged we report not-wired rather than hang.
    func poll() {
        piWired = false
        lastError = nil

        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/local/bin/jack_lsp")
        var env = ProcessInfo.processInfo.environment
        env["JACK_NO_START_SERVER"] = "1"
        p.environment = env
        p.standardOutput = Pipe()
        p.standardError = Pipe()

        let sem = DispatchSemaphore(value: 0)
        queue.asyncAfter(deadline: .now() + 5) { sem.signal() } // watchdog
        do {
            try p.run()
        } catch {
            lastError = "jack_lsp not found: \(error.localizedDescription)"
            DispatchQueue.main.async { [weak self] in self?.onUpdate?() }
            return
        }
        DispatchQueue.global(qos: .utility).async {
            p.waitUntilExit()
            sem.signal()
        }
        _ = sem.wait(timeout: .now() + 6)
        if p.isRunning { p.terminate() }

        if p.terminationStatus == 0, let out = (p.standardOutput as? Pipe) {
            let data = out.fileHandleForReading.readDataToEndOfFile()
            let text = String(data: data, encoding: .utf8) ?? ""
            piWired = text.split(separator: "\n").contains {
                $0.contains("from_slave") || $0.contains("pistomp")
            }
        } else if p.terminationStatus != 0 {
            lastError = "jack_lsp exit \(p.terminationStatus)"
        }
        DispatchQueue.main.async { [weak self] in self?.onUpdate?() }
    }

    /// Synchronous one-shot for the diagnostics dump. Returns (output, status).
    static func runJackLsp(connect: Bool) -> (String, Int32) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/local/bin/jack_lsp")
        var env = ProcessInfo.processInfo.environment
        env["JACK_NO_START_SERVER"] = "1"
        p.environment = env
        if connect { p.arguments = ["-c"] }
        let out = Pipe(); let err = Pipe()
        p.standardOutput = out; p.standardError = err
        do {
            try p.run()
            p.waitUntilExit()
        } catch {
            return ("exec failed: \(error.localizedDescription)", -1)
        }
        let o = String(data: out.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        let e = String(data: err.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        return (o + e, p.terminationStatus)
    }
}
