import Foundation

/// Bounded subprocess execution.
///
/// Every subprocess the Companion spawns is a diagnostic or a control action —
/// none of them is allowed to wedge the app. `run` returns within
/// `timeout + 3*grace` — the budget, then a SIGTERM wait, a SIGKILL wait, and
/// a final wait for end-of-output — regardless of what the child does:
///
///   * output is drained via `readabilityHandler` rather than a blocking read,
///     so a child that fills the 64 KiB pipe buffer can't deadlock us before
///     the watchdog fires, and a child that is killed mid-stream still yields
///     whatever it had already written;
///   * the watchdog escalates SIGTERM → SIGKILL;
///   * the final collect is bounded too, so a grandchild holding the pipe open
///     costs us `grace` and no more.
enum ProcessRunner {

    struct Result {
        var stdout = ""
        var stderr = ""
        /// Exit status, or the negated signal number for a killed child.
        /// `nil` when the process never launched or never reaped.
        var status: Int32?
        /// True when the watchdog had to intervene.
        var timedOut = false
        /// Set instead of `status` when `posix_spawn` itself failed.
        var launchError: String?

        var combined: String {
            stdout + (stderr.isEmpty ? "" : stderr)
        }

        /// Rendering for the diagnostics dump: a status banner (so a timeout
        /// is never mistaken for empty output) plus the child's own bytes.
        func report(includeExit: Bool) -> String {
            if let launchError { return "[exec failed: \(launchError)]\n" }
            var banner = ""
            if timedOut {
                banner = "[timed out — killed after budget]\n"
            } else if includeExit {
                banner = "[exit \(status.map(String.init) ?? "?")]\n"
            }
            return banner + combined
        }
    }

    /// Runs `path` to completion or to the timeout, whichever comes first.
    /// Safe to call from any queue; blocks the caller for at most
    /// `timeout + 3*grace`.
    static func run(_ path: String, args: [String] = [], env: [String: String]? = nil,
                    timeout: TimeInterval, grace: TimeInterval = 1.0) -> Result {
        var result = Result()

        let p = Process()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = args
        if let env { p.environment = env }
        let outPipe = Pipe(), errPipe = Pipe()
        p.standardOutput = outPipe
        p.standardError = errPipe
        // Never inherit the app's stdin: a child that reads it would block
        // forever on a handle nobody writes.
        p.standardInput = FileHandle.nullDevice

        let outSink = Sink(), errSink = Sink()
        attach(outPipe.fileHandleForReading, to: outSink)
        attach(errPipe.fileHandleForReading, to: errSink)

        let reaped = Flag()
        let exited = DispatchSemaphore(value: 0)
        p.terminationHandler = { _ in
            reaped.set()
            exited.signal()
        }

        do {
            try p.run()
        } catch {
            outSink.detach(outPipe.fileHandleForReading)
            errSink.detach(errPipe.fileHandleForReading)
            result.launchError = error.localizedDescription
            return result
        }

        if exited.wait(timeout: .now() + timeout) == .timedOut {
            result.timedOut = true
            // Graceful first: a SIGTERMed jack_lsp still flushes what it had.
            if !reaped.isSet { p.terminate() }
            if exited.wait(timeout: .now() + grace) == .timedOut {
                if !reaped.isSet { kill(p.processIdentifier, SIGKILL) }
                _ = exited.wait(timeout: .now() + grace)
            }
        }

        // EOF on both pipes normally lands right after the child exits. If it
        // doesn't (an inherited fd in a grandchild), take the partial output
        // rather than waiting on it.
        // One shared deadline, not one each: a backgrounded grandchild holds
        // both write ends, and waiting on them in series would double the cost.
        let eofDeadline = DispatchTime.now() + grace
        _ = outSink.eof.wait(timeout: eofDeadline)
        _ = errSink.eof.wait(timeout: eofDeadline)
        outSink.detach(outPipe.fileHandleForReading)
        errSink.detach(errPipe.fileHandleForReading)

        result.stdout = outSink.text
        result.stderr = errSink.text
        if reaped.isSet {
            result.status = p.terminationReason == .uncaughtSignal
                ? -p.terminationStatus
                : p.terminationStatus
        }
        return result
    }

    // MARK: - plumbing

    private static func attach(_ handle: FileHandle, to sink: Sink) {
        handle.readabilityHandler = { h in
            let d = h.availableData
            if d.isEmpty {
                h.readabilityHandler = nil
                sink.finish()
            } else {
                sink.append(d)
            }
        }
    }

    /// Accumulates one stream. Written from the FileHandle's internal queue,
    /// read from the caller's — hence the lock.
    private final class Sink {
        let eof = DispatchSemaphore(value: 0)
        private let lock = NSLock()
        private var data = Data()
        private var done = false

        func append(_ d: Data) {
            lock.lock(); data.append(d); lock.unlock()
        }

        func finish() {
            lock.lock()
            let already = done
            done = true
            lock.unlock()
            if !already { eof.signal() }
        }

        var text: String {
            lock.lock(); defer { lock.unlock() }
            return String(data: data, encoding: .utf8) ?? ""
        }

        /// Drops the handler (breaking the DispatchSource's retain) and marks
        /// the stream finished so a late EOF can't signal into a dead wait.
        func detach(_ handle: FileHandle) {
            handle.readabilityHandler = nil
            finish()
        }
    }

    private final class Flag {
        private let lock = NSLock()
        private var value = false
        func set() { lock.lock(); value = true; lock.unlock() }
        var isSet: Bool { lock.lock(); defer { lock.unlock() }; return value }
    }
}
