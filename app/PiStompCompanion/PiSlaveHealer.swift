import Foundation

/// Restarts the pi's netJACK2 slave when the Mac stack is up and the slave is
/// not, without touching the Mac stack.
///
/// **The gap this closes.** `jackbridge-ctl start` brings up both ends, but the
/// pi half is best-effort by design: an unreachable pi must never block a local
/// start (a cable can be out, and the Mac's jackd is still useful recording
/// nothing). So `pi_service start` logs "pi unreachable — skipping" and moves
/// on. Nothing ever retried it. Two ordinary sequences therefore ended in a
/// permanent yellow badge that only `Restart JackBridge` cleared:
///
///   1. The Mac's agents start while the pi is still booting. The one start we
///      were ever going to send lands on a pi that is not answering ssh yet.
///   2. The pedal is power-cycled while the Mac keeps running. The unit is
///      deliberately not `enable`d at boot, so the pi comes back with the
///      slave inactive and no one asks it to start.
///
/// Neither is a fault a user can be expected to diagnose, and both present
/// identically: link down, ports at zero, everything else healthy.
///
/// **The Mac is the source of truth.** There is no pi-side "off" this has to
/// negotiate with: if the Mac stack is running, the slave is supposed to be
/// running too, and this closes the gap between those two facts. That is also
/// why no separate intent flag exists — a live daemon heartbeat *is* the
/// intent. `jackbridge-ctl stop` tears the agents down, `.stackDown` follows,
/// and healing stops with it, so this can never resurrect a link the user
/// deliberately stopped.
///
/// **Why it never bounces the Mac side.** `pi-start` is `systemctl start` over
/// ssh and nothing else. Kicking the agents instead would flip `DeviceIsAlive`
/// to 0, and a DAW that released the device does not re-acquire it — the user
/// pays for our repair with a device re-select in the middle of a take
/// (CLAUDE.md §4b). The narrow verb is the whole point.
final class PiSlaveHealer {

    /// How long the bad condition must hold before the first attempt. Long
    /// enough that ordinary churn — the seconds between the agents coming up
    /// and the slave's ports wiring, a netadapter in-process resync — heals
    /// itself without us, short enough that a user watching the menu bar sees
    /// it recover rather than giving up and reaching for Restart.
    private static let dwell: TimeInterval = 10

    /// Backoff bounds. A pi that is reachable over ssh but cannot start the
    /// unit (no wired interface for the multicast pin, say) is a fault we
    /// cannot fix by asking again sooner; keep the retry cheap and rare rather
    /// than spending the audio host's jitter budget on ssh handshakes.
    private static let baseBackoff: TimeInterval = 10
    private static let maxBackoff: TimeInterval = 300

    /// Injected so this stays testable and so AppDelegate keeps ownership of
    /// how `jackbridge-ctl` is spawned. Called on the main queue; must call
    /// its completion on the main queue.
    private let runCtl: (String, @escaping (Bool) -> Void) -> Void

    private var eligibleSince: Date?
    private var nextAttemptAt = Date.distantPast
    private var backoff = PiSlaveHealer.baseBackoff
    private var inFlight = false

    init(runCtl: @escaping (String, @escaping (Bool) -> Void) -> Void) {
        self.runCtl = runCtl
    }

    /// Feed every published state. Main queue only — same queue `runCtl`'s
    /// completion lands on, which is what keeps the counters race-free
    /// without a lock.
    func note(_ state: StatusMonitor.State) {
        dispatchPrecondition(condition: .onQueue(.main))

        guard eligible(state) else {
            // Ports back (or the stack intentionally down): forget the episode
            // entirely, so the next one starts from the short backoff instead
            // of inheriting a long one from an unrelated failure.
            eligibleSince = nil
            backoff = Self.baseBackoff
            nextAttemptAt = .distantPast
            return
        }

        let now = Date()
        guard let since = eligibleSince else {
            eligibleSince = now
            return
        }
        guard !inFlight,
              now.timeIntervalSince(since) >= Self.dwell,
              now >= nextAttemptAt else { return }

        attempt()
    }

    /// The stack is up, the pi answers, and the slave is not there.
    ///
    /// `slavePortsConnected` is the right signal rather than
    /// `health == .noAudioFromPi`: that case only arises once a DAW has the
    /// device open (`driverStatus == STARTED`). The commonest shape of this
    /// bug is nobody having opened the device yet — the user turns the pedal
    /// on, looks at the menu bar, and waits — which reads as `.piUnreachable`
    /// or `.linkedIdle`. Keying on the port count covers every one of them.
    private func eligible(_ state: StatusMonitor.State) -> Bool {
        switch state.health {
        case .stackDown, .protocolMismatch:
            // Nothing to heal *to*, and a protocol mismatch needs a reinstall,
            // not a retry loop.
            return false
        case .noAudioFromPi, .streaming, .startedIdle, .linkedIdle, .piUnreachable:
            break
        }
        // No wired path yet means the pi's own ExecStartPre route pin would
        // fail and trip its restart limiter. Wait for the cable instead of
        // burning the pi's five allowed starts.
        guard state.jackCondition != .waitingForNetwork else { return false }
        guard state.piReachable else { return false }
        return state.snapshot.slavePortsConnected == 0
    }

    private func attempt() {
        inFlight = true
        NSLog("PiSlaveHealer: slave down with the stack up — jackbridge-ctl pi-start")
        runCtl("pi-start") { [weak self] ok in
            guard let self else { return }
            self.inFlight = false
            if ok {
                // Not "healed" — `systemctl start` returning 0 only means the
                // unit was asked. The ports are the verdict, and the next
                // `note` that sees them wired resets everything. Until then
                // keep backing off, or a unit that starts and immediately
                // fails would be re-asked every dwell forever.
                NSLog("PiSlaveHealer: pi-start accepted; waiting for ports")
            } else {
                NSLog("PiSlaveHealer: pi-start failed; retrying in \(Int(self.backoff))s")
            }
            self.nextAttemptAt = Date().addingTimeInterval(self.backoff)
            self.backoff = min(self.backoff * 2, Self.maxBackoff)
            // Restart the dwell so the next attempt needs a fresh, sustained
            // bad reading rather than firing the instant the backoff expires.
            self.eligibleSince = Date()
        }
    }
}
