import Foundation

/// Polls `jack_lsp` for the pi's netJACK2 slave ports.
///
/// Ports live in JACK's graph, not in shm, so this shells out at 0.5 Hz.
/// `JACK_NO_START_SERVER=1` is mandatory: without it jack_lsp auto-spawns a
/// stray default jackd and can trip a TCC microphone prompt (same reason
/// `jackbridge/installer/jackd-launch` exports it) — `JackTools.environment`
/// carries it, and `JackTools.jackLsp` is the one agreed-on tool path.
final class JackGraphMonitor {
    struct Result {
        /// True when the JACK graph contains `from_slave` / `pistomp` ports.
        var piWired = false
        var error: String?
    }

    /// Called on the monitor's own queue, not on main.
    var onUpdate: ((Result) -> Void)?

    private let queue = DispatchQueue(label: "com.treefallsound.companion.jackgraph", qos: .utility)
    private var timer: DispatchSourceTimer?

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

    /// Runs jack_lsp synchronously on the monitor's queue. Bounded at 5 s —
    /// if jackd's socket is wedged we report not-wired rather than hang, and
    /// the poll interval keeps the next attempt coming.
    private func poll() {
        onUpdate?(Self.probe())
    }

    /// One bounded `jack_lsp` run, reduced to the wired/not-wired answer.
    static func probe(timeout: TimeInterval = 5) -> Result {
        let r = ProcessRunner.run(JackTools.jackLsp, env: JackTools.environment, timeout: timeout)
        if let launchError = r.launchError {
            return Result(error: "jack_lsp not runnable at \(JackTools.jackLsp): \(launchError)")
        }
        if r.timedOut {
            return Result(error: "jack_lsp timed out after \(Int(timeout))s")
        }
        guard r.status == 0 else {
            return Result(error: "jack_lsp exit \(r.status.map(String.init) ?? "?")")
        }
        let wired = r.stdout.split(separator: "\n").contains {
            $0.contains("from_slave") || $0.contains("pistomp")
        }
        return Result(piWired: wired)
    }

    /// Bounded one-shot for the diagnostics dump.
    static func runJackLsp(connect: Bool, timeout: TimeInterval = 5) -> ProcessRunner.Result {
        ProcessRunner.run(JackTools.jackLsp, args: connect ? ["-c"] : [],
                          env: JackTools.environment, timeout: timeout)
    }
}
