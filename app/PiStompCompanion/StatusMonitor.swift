import Foundation

/// Aggregates the three status sources into one `StackState` the UI renders.
final class StatusMonitor {
    enum Health: Equatable {
        case protocolMismatch(UInt64)     // shm protocolVersion != 5
        case streaming(UInt64, UInt64)    // sampleRate, nFrames — audio flowing
        case startedIdle                  // driverStatus == STARTED but HAL head not advancing
        case linkedIdle                   // ports wired, driverStatus != STARTED
        case piUnreachable                // normal state when cable is out
        case stackDown                    // shm absent, or no daemon heartbeat
    }

    struct State {
        var health: Health = .stackDown
        var detailLine = ""
        var piReachable = false
        var resolvedAddress: String?
        var piWired = false
        var shmAttached = false
        var shmError: String?
        var snapshot = ShmSnapshot()
    }

    private(set) var state = State()

    private let shm = ShmReader()
    private let graph = JackGraphMonitor()
    private let reach = ReachabilityMonitor()

    var onUpdate: ((State) -> Void)?

    // Heartbeat bookkeeping: deltas across successive 200 ms polls. The
    // daemon heartbeats once per JACK cycle (~1.3 ms at 48k/64), so any
    // delta at all means "alive" — tighter than the plan's 400 ms and
    // still discriminates a stall.
    private var lastDaemonAlive: UInt64 = 0
    private var daemonBeating = false
    private var lastReadHead: UInt64 = 0
    private var readHeadAdvancing = false
    private var lastSeed: UInt64 = 0

    private let shmQueue = DispatchQueue(label: "com.jackbridge.companion.shm", qos: .utility)
    private var shmTimer: DispatchSourceTimer?
    private var attachTimer: DispatchSourceTimer?

    func start() {
        graph.onUpdate = { [weak self] in self?.recompute() }
        graph.start(interval: 2.0)
        reach.onUpdate = { [weak self] in self?.recompute() }
        reach.start(interval: 15.0)

        // 5 Hz shm poll.
        let t = DispatchSource.makeTimerSource(queue: shmQueue)
        t.schedule(deadline: .now(), repeating: 0.2)
        t.setEventHandler { [weak self] in self?.pollShm() }
        shmTimer = t
        t.resume()

        // If shm vanished (stack stopped), retry attach every 2 s so the
        // app recovers when the stack comes back.
        let a = DispatchSource.makeTimerSource(queue: shmQueue)
        a.schedule(deadline: .now() + 2, repeating: 2.0)
        a.setEventHandler { [weak self] in
            guard let self, !self.shm.attached else { return }
            do {
                try self.shm.attach()
                self.state.shmError = nil
            } catch {
                self.state.shmError = String(describing: error)
            }
        }
        attachTimer = a
        a.resume()
    }

    private func pollShm() {
        if shm.attached {
            state.shmAttached = true
            state.shmError = nil
            state.snapshot = shm.snapshot()
        } else {
            state.shmAttached = false
        }

        let snap = state.snapshot
        daemonBeating = snap.daemonAlive != lastDaemonAlive
        lastDaemonAlive = snap.daemonAlive
        readHeadAdvancing = snap.halInputReadHead != lastReadHead
        lastReadHead = snap.halInputReadHead
        // Seed churn (HAL re-anchor) noted for diagnostics; not surfaced yet.
        _ = snap.seed != lastSeed
        lastSeed = snap.seed

        DispatchQueue.main.async { [weak self] in self?.recompute() }
    }

    /// Merges everything into `state` and fires onUpdate. Must run on main.
    func recompute() {
        assert(Thread.isMainThread)
        let snap = state.snapshot

        state.piReachable = reach.lastResult.reachable
        state.resolvedAddress = reach.lastResult.resolvedAddress
        state.piWired = graph.piWired

        let health: Health
        if state.shmAttached, snap.protocolVersion != 0,
           snap.protocolVersion != ShmSnapshot.expectedProtocolVersion {
            health = .protocolMismatch(snap.protocolVersion)
        } else if !state.shmAttached || !daemonBeating {
            // No heartbeat is indistinguishable from "stopped" via shm
            // alone (a stale region persists after bootout). Dim, don't
            // cry red — red is reserved for the protocol mismatch, which
            // actually requires user action.
            health = .stackDown
        } else if readHeadAdvancing {
            health = .streaming(snap.halSampleRate, snap.halNFrames)
        } else if snap.driverStatus == ShmSnapshot.driverStatusStarted {
            health = .startedIdle
        } else if state.piWired {
            health = .linkedIdle
        } else {
            health = .piUnreachable
        }
        state.health = health
        state.detailLine = detailLine(for: health)

        onUpdate?(state)
    }

    private func detailLine(for h: Health) -> String {
        switch h {
        case .protocolMismatch(let v):
            return "Reinstall required — shm protocol \(v) != \(ShmSnapshot.expectedProtocolVersion)"
        case .streaming(let rate, let nframes):
            return "Streaming — \(rate / 1000) kHz / \(nframes) frames"
        case .startedIdle:
            return "IO started, idle"
        case .linkedIdle:
            return "Linked, idle"
        case .piUnreachable:
            return state.piReachable ? "pi-Stomp reachable, not in JACK graph" : "pi-Stomp not found"
        case .stackDown:
            return "JackBridge stack down"
        }
    }
}
