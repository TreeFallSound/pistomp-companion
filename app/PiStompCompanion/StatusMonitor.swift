import Foundation

/// Aggregates the three status sources into one `StackState` the UI renders.
///
/// **Threading contract.** Every field of `State` is owned by `stateQueue` and
/// touched nowhere else: the shm poll and the graph / reachability callbacks
/// all funnel through it. The UI never reads this
/// object — `onUpdate` hands the main queue an immutable copy of `State`, and
/// that copy is the only thing AppDelegate is allowed to render from. Without
/// that split, the 5 Hz poll writing `state` while the main queue read it was
/// a straight data race, and could show a half-updated mix of two polls.
final class StatusMonitor {
    enum Health: Equatable {
        case protocolMismatch(UInt64)     // shm protocolVersion != 5
        case streaming(UInt64, UInt64)    // sampleRate, nFrames — audio flowing
        case startedIdle                  // driverStatus == STARTED but HAL head not advancing
        case linkedIdle                   // ports wired, driverStatus != STARTED
        case piUnreachable                // normal state when cable is out
        case stackDown                    // shm absent, or no daemon heartbeat
    }

    enum JackCondition: Equatable {
        case none
        case ourServer
        case differentProgram
    }

    /// A consistent snapshot of everything the UI and the diagnostics dump
    /// need. Value type on purpose: publishing is a copy, not a reference.
    struct State {
        var health: Health = .stackDown
        var detailLine = ""
        var jackCondition: JackCondition = .none
        var piReachable = false
        var resolvedAddress: String?
        var reachedVia: String?
        var piWired = false
        var jackGraphError: String?
        var shmAttached = false
        var shmError: String?
        var snapshot = ShmSnapshot()
    }

    private let shm = ShmReader()
    private let graph = JackGraphMonitor()
    private let reach = ReachabilityMonitor()

    /// Delivered on the main queue. Set before `start()`.
    var onUpdate: ((State) -> Void)?

    /// `stateQueue`-owned. Serial, so a poll and a callback can never
    /// interleave their writes.
    private let stateQueue = DispatchQueue(label: "com.treefallsound.companion.state", qos: .utility)
    private var state = State()

    // Heartbeat bookkeeping: deltas across successive 200 ms polls. The
    // daemon heartbeats once per JACK cycle (~1.3 ms at 48k/64), so any
    // delta at all means "alive" — tighter than the plan's 400 ms and
    // still discriminates a stall. All stateQueue-owned.
    private var lastDaemonAlive: UInt64 = 0
    private var daemonBeating = false
    private var lastReadHead: UInt64 = 0
    private var readHeadAdvancing = false
    private var lastSeed: UInt64 = 0
    /// False until we have two consecutive polls off the *same* mapping.
    /// A delta against a zeroed baseline is not evidence of motion — it's
    /// what the first poll after an attach or a remap always looks like.
    private var haveBaseline = false

    private var shmTimer: DispatchSourceTimer?

    func start() {
        graph.onUpdate = { [weak self] result in
            guard let self else { return }
            self.stateQueue.async { self.apply(graph: result) }
        }
        graph.start(interval: 2.0)

        reach.onUpdate = { [weak self] result in
            guard let self else { return }
            self.stateQueue.async { self.apply(reach: result) }
        }
        reach.start(interval: 15.0)

        // 5 Hz shm poll.
        let t = DispatchSource.makeTimerSource(queue: stateQueue)
        t.schedule(deadline: .now(), repeating: 0.2)
        t.setEventHandler { [weak self] in self?.pollShm() }
        shmTimer = t
        t.resume()

    }

    func stop() {
        shmTimer?.cancel(); shmTimer = nil
        graph.stop()
        reach.stop()
    }

    // MARK: - shm

    /// Re-resolves `/JackBridge` and remaps it. Doubles as the attach retry
    /// when the stack is down: a poll that finds nothing is the same code
    /// path as a poll that finds a new region.
    ///
    /// This has to happen every poll rather than once, because a mapping
    /// outlives the name it came from — see `ShmReader.attach()`. The
    /// symptom it prevents is the nastiest kind: not an error, but a live-
    /// looking display frozen on an orphaned region after an upgrade.
    private func remap() {
        dispatchPrecondition(condition: .onQueue(stateQueue))
        do {
            try shm.attach()
            state.shmAttached = true
            state.shmError = nil
        } catch {
            shm.detach()
            state.shmAttached = false
            state.shmError = String(describing: error)
        }
    }

    private func pollShm() {
        dispatchPrecondition(condition: .onQueue(stateQueue))
        remap()
        pollJackStatus()

        if shm.attached {
            let snap = shm.snapshot()
            state.snapshot = snap
            // Both counters only ever climb while one region lives, so a
            // decrease means the region or the daemon behind it was replaced.
            // The old baseline describes something that no longer exists —
            // keep the jump from reading as motion for one poll.
            if snap.daemonAlive < lastDaemonAlive || snap.halInputReadHead < lastReadHead {
                haveBaseline = false
            }
            daemonBeating = haveBaseline && snap.daemonAlive != lastDaemonAlive
            readHeadAdvancing = haveBaseline && snap.halInputReadHead != lastReadHead
            lastDaemonAlive = snap.daemonAlive
            lastReadHead = snap.halInputReadHead
            // Seed churn (HAL re-anchor) noted for diagnostics; not surfaced yet.
            _ = snap.seed != lastSeed
            lastSeed = snap.seed
            haveBaseline = true
        } else {
            // Never render half of a dead mapping: drop the values along with
            // the mapping so "not attached" and the numbers can't disagree.
            state.snapshot = ShmSnapshot()
            haveBaseline = false
            daemonBeating = false
            readHeadAdvancing = false
            lastDaemonAlive = 0
            lastReadHead = 0
            lastSeed = 0
        }

        recompute()
    }

    private func pollJackStatus() {
        dispatchPrecondition(condition: .onQueue(stateQueue))
        let path = ProcessInfo.processInfo.environment["JACKBRIDGE_STATUS_FILE"]
            ?? "/tmp/jackbridge-jackd.status"
        guard let text = try? String(contentsOfFile: path, encoding: .utf8) else {
            state.jackCondition = .none
            return
        }
        switch text.trimmingCharacters(in: .whitespacesAndNewlines) {
        case "owned":
            state.jackCondition = .ourServer
        case "foreign":
            state.jackCondition = .differentProgram
        default:
            state.jackCondition = .none
        }
    }

    // MARK: - other sources

    private func apply(graph result: JackGraphMonitor.Result) {
        dispatchPrecondition(condition: .onQueue(stateQueue))
        state.piWired = result.piWired
        state.jackGraphError = result.error
        recompute()
    }

    private func apply(reach result: ReachabilityMonitor.Result) {
        dispatchPrecondition(condition: .onQueue(stateQueue))
        state.piReachable = result.reachable
        state.resolvedAddress = result.resolvedAddress
        state.reachedVia = result.via
        recompute()
    }

    // MARK: - publishing

    /// Derives health from `state` and publishes an immutable copy.
    /// `stateQueue` only.
    private func recompute() {
        dispatchPrecondition(condition: .onQueue(stateQueue))
        let snap = state.snapshot

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

        let published = state
        DispatchQueue.main.async { [weak self] in self?.onUpdate?(published) }
    }

    private func detailLine(for h: Health) -> String {
        switch state.jackCondition {
        case .ourServer:
            return "JackBridge waits for JACK"
        case .differentProgram:
            return "A different program uses JACK"
        case .none:
            break
        }
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
