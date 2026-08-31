import Foundation

/// Snapshot of the /JackBridge shm control region. Field offsets are the
/// `JB_OFF_*` constants from `jackbridge/shared/JackBridge.h` — protocol version 12.
/// Every field is a plain aligned uint64_t (compile-time asserted on the
/// C++ side), so a read of UInt64 at these offsets is exact.
struct ShmSnapshot {
    var numberTimeStamps: UInt64 = 0
    var zeroHostTime: UInt64 = 0
    var seed: UInt64 = 0
    var syncMode: UInt64 = 0
    var bufferSize: UInt64 = 0
    var driverStatus: UInt64 = 0
    var protocolVersion: UInt64 = 0
    var daemonAlive: UInt64 = 0
    var halAnchorSeq: UInt64 = 0
    var halAnchorHostTime: UInt64 = 0
    var halAnchorSampleTime: UInt64 = 0
    var halInputReadHead: UInt64 = 0
    var halOutputWriteHead: UInt64 = 0
    var halNFrames: UInt64 = 0
    var halSampleRate: UInt64 = 0
    var readFrameNumber: [UInt64] = [0, 0]
    var writeFrameNumber: [UInt64] = [0, 0]
    /// JACK timing the daemon discovered from the Pi; inputs to the advertised
    /// latency model (see docs/LATENCY-MODEL.md).
    var jackPeriodFrames: UInt64 = 0
    var jackSampleRate: UInt64 = 0
    /// Protocol-8 self-healing fields. See `JB_OFF_*` in `jackbridge/shared/JackBridge.h`.
    /// How many of the daemon's 6 slave-facing ports currently have a live
    /// connection. Drops to 0 the moment jackd reaps a departed pi.
    var slavePortsConnected: UInt64 = 0
    /// Monotonic daemon xrun count. Watch its *rate*: a fast ramp is netmanager
    /// stalling ~2 s per cycle against a pi that is no longer there.
    var daemonXRuns: UInt64 = 0
    /// Shared fault bitfield. Bit 0 means the driver feeds silence; bit 1
    /// means the daemon's ring geometry is unsafe for the current N/P/J.
    var driverFault: UInt64 = 0
    /// Echo of the app's last re-anchor nonce (Phase 4). The driver re-arms its
    /// liveness state on a new nonce; the daemon re-anchors the timeline.
    var resyncRequest: UInt64 = 0
    /// Protocol-10. The netadapter loop pair the pi is running, published by
    /// the daemon from config.plist.
    var netLatencyCycles: UInt64 = 0
    var netRingFrames: UInt64 = 0
    /// Protocol-10 timeline health, from the daemon's last ~5 s window. Both
    /// were os_log-only before, which is how a stack could be hours out of
    /// anchor while every field here read healthy. Nonzero in steady state
    /// means the timeline is not holding.
    var healthDeltaMax: UInt64 = 0
    var healthSnaps: UInt64 = 0
    /// Re-anchors since the daemon started. Advancing on its own means the
    /// automatic divergence recovery is firing.
    var reanchorCount: UInt64 = 0
    /// Cadence. The daemon positions both rings from the HAL's heads, and
    /// these say whether its own cycles ran 1:1 against them. The read side
    /// is the audible one: the daemon's read is destructive, so a repeated
    /// cycle sends the pi zeroed slots — silence inside correctly paced
    /// audio, heard as a haze rather than as a click. Read as rates: a few
    /// from a start transient mean nothing, a counter climbing while audio
    /// plays is the fault.
    var dupReadCycles: UInt64 = 0
    var skipReadFrames: UInt64 = 0
    var dupWriteCycles: UInt64 = 0
    var skipWriteFrames: UInt64 = 0
    /// Snap-to-target corrections of the free-running read cursor.
    /// Occasional snaps absorb scheduling jitter; a steady climb means the
    /// two clock rates differ.
    var recvResyncs: UInt64 = 0

    /// Hardcoded on purpose, and it must stay that way. The offsets below are
    /// hand-copied literals the compiler cannot check against
    /// shared/JackBridge.h, so this constant is the tripwire that forces
    /// someone to re-verify them on a layout change. Deriving it from the
    /// header would let the app recompile past a bump and read stale offsets.
    static let expectedProtocolVersion: UInt64 = 12
    static let driverStatusStarted: UInt64 = 2
    /// Full complement of daemon slave ports (NUM_INPUT_CHANNELS + NUM_OUTPUT_CHANNELS).
    static let slavePortsFull: UInt64 = 6
    static let faultDeviceNotAlive: UInt64 = 1 << 0
    static let faultBadRingGeometry: UInt64 = 1 << 1
}

/// Read-only mapper of the /JackBridge POSIX shm region.
///
/// NEVER opens read-write: the daemon + HAL own the region and a writer fd
/// here would contend with them. O_RDONLY + PROT_READ only.
final class ShmReader {
    enum ShmError: Error, CustomStringConvertible {
        case openFailed(Int32)
        case fstatFailed(Int32)
        case wrongSize(Int)
        case mapFailed(Int32)

        var description: String {
            switch self {
            case .openFailed(let e): return "shm_open failed: \(String(cString: strerror(e)))"
            case .fstatFailed(let e): return "fstat failed: \(String(cString: strerror(e)))"
            case .wrongSize(let s): return "shm size \(s) != expected \(ShmReader.regionsSize)"
            case .mapFailed(let e): return "mmap failed: \(String(cString: strerror(e)))"
            }
        }
    }

    // Must match jackbridge/shared/JackBridge.h
    static let regionsSize = 0x30000   // JACK_SHMSIZE = 0x10000*2 + 0x10000
    static let regionSize = 0x20000    // REGSMAP_SIZE

    /// Region name. Parameterized only so tests can exercise attach /
    /// remap / vanish against a scratch region without touching the live
    /// one — production always uses the default.
    let name: String
    init(name: String = "/JackBridge") { self.name = name }

    private var fd: Int32 = -1
    private var base: UnsafeMutableRawPointer?
    private(set) var attached = false

    deinit { detach() }

    /// Resolve `/JackBridge` and map it, replacing any previous mapping.
    ///
    /// Called every poll, not once: on XNU a mapping outlives the name it
    /// came from. `shm_unlink` + recreate — which is what a package upgrade,
    /// a `jb-rmshm`, or a protocol bump does — leaves the old mapping fully
    /// readable and frozen at its last values, so a Companion that mapped
    /// once would keep reporting plausible numbers from an orphan forever.
    ///
    /// There is no cheaper check: `fstat` on a POSIX shm object under XNU
    /// reports `st_dev == 0` and `st_ino == 0` (measured, not assumed), so
    /// there is no object identity to compare against. `st_size` is the only
    /// meaningful field, and it does not change across a recreate. Hence
    /// re-open + re-map — a few microseconds at 5 Hz, in a menu-bar app that
    /// already forks `jack_lsp` every two seconds.
    func attach() throws {
        // shm_open is variadic (unavailable in Swift) — the C shim in
        // ShmShim.c opens it read-only for us.
        let fd = jb_shm_open_ro(name)
        if fd < 0 { throw ShmError.openFailed(errno) }

        var st = stat()
        if fstat(fd, &st) < 0 {
            close(fd)
            throw ShmError.fstatFailed(errno)
        }
        if st.st_size != Self.regionsSize {
            close(fd)
            throw ShmError.wrongSize(Int(st.st_size))
        }

        let map = mmap(nil, Self.regionSize, PROT_READ, MAP_SHARED, fd, 0)
        if map == MAP_FAILED {
            close(fd)
            throw ShmError.mapFailed(errno)
        }

        // Swap only after the new mapping is in hand: a failed remap must
        // leave the previous one intact rather than blinding the UI.
        detach()
        self.fd = fd
        self.base = map
        attached = true
    }

    func detach() {
        if let base { munmap(base, Self.regionSize) }
        if fd >= 0 { close(fd) }
        base = nil
        fd = -1
        attached = false
    }

    private func field(_ offset: Int) -> UInt64 {
        guard let base else { return 0 }
        // Every field is an aligned uint64_t atomic; a single load of a
        // naturally aligned 64-bit value is atomic on arm64/x86_64.
        return base.load(fromByteOffset: offset, as: UInt64.self)
    }

    /// One consistent-ish read of the control region. Individual fields are
    /// atomic; cross-field consistency is not needed for status display.
    func snapshot() -> ShmSnapshot {
        var s = ShmSnapshot()
        guard base != nil else { return s }
        s.numberTimeStamps    = field(0x100)
        s.zeroHostTime        = field(0x108)
        s.seed                = field(0x110)
        s.syncMode            = field(0x118)
        s.bufferSize          = field(0x120)
        s.driverStatus        = field(0x128)
        s.protocolVersion     = field(0x130)
        s.daemonAlive         = field(0x138)
        s.halAnchorSeq        = field(0x140)
        s.halAnchorHostTime   = field(0x148)
        s.halAnchorSampleTime = field(0x150)
        s.halInputReadHead    = field(0x158)
        s.halOutputWriteHead  = field(0x160)
        s.halNFrames          = field(0x168)
        s.halSampleRate       = field(0x170)
        s.readFrameNumber     = [field(0x180), field(0x190)]
        s.writeFrameNumber    = [field(0x188), field(0x198)]
        s.jackPeriodFrames    = field(0x1a0)
        s.jackSampleRate      = field(0x1a8)
        s.slavePortsConnected = field(0x1b0)
        s.daemonXRuns         = field(0x1b8)
        s.driverFault         = field(0x1c0)
        s.resyncRequest       = field(0x1c8)
        s.netLatencyCycles    = field(0x1d8)
        s.netRingFrames       = field(0x1e0)
        s.healthDeltaMax      = field(0x1e8)
        s.healthSnaps         = field(0x1f0)
        s.reanchorCount       = field(0x1f8)
        s.dupReadCycles       = field(0x280)
        s.skipReadFrames      = field(0x288)
        s.dupWriteCycles      = field(0x290)
        s.skipWriteFrames     = field(0x298)
        s.recvResyncs         = field(0x2a0)
        return s
    }
}

/// Read-WRITE mapper used for exactly one thing: the app's Repair (light)
/// item writes a nonce into JB_OFF_RESYNC_REQUEST to ask the driver to
/// re-anchor without a coreaudiod bounce.
///
/// Deliberately a *separate* class from ShmReader. ShmReader is strictly
/// read-only so status polling can never contend with daemon/HAL writes,
/// and that contract is worth protecting — if a read/write `attach()` ever
/// appeared on it, a careless caller could corrupt live fields. The writer
/// only ever touches one word (offset 0x1c8), which the driver consumes
/// and clears.
final class ShmWriter {
    enum WriteError: Error, CustomStringConvertible {
        case openFailed(Int32)
        case wrongSize(Int)
        case mapFailed(Int32)

        var description: String {
            switch self {
            case .openFailed(let e): return "shm_open (rw) failed: \(String(cString: strerror(e)))"
            case .wrongSize(let s): return "shm size \(s) != expected \(ShmReader.regionsSize)"
            case .mapFailed(let e): return "mmap (rw) failed: \(String(cString: strerror(e)))"
            }
        }
    }

    private let name: String
    init(name: String = "/JackBridge") { self.name = name }

    /// Store `nonce` at JB_OFF_RESYNC_REQUEST. The driver compares against
    /// its last-observed value each GetZeroTimeStamp; a different value
    /// triggers one re-anchor, then the driver clears its copy.
    ///
    /// The read-modify-write semantics live entirely on the driver side —
    /// this function only stores. The nonce space is just "any new value",
    /// so callers pass a counter or UInt64.random(in:).
    func pokeResync(nonce: UInt64) throws {
        // Same variadic-shm_open constraint as the reader: go through the
        // C shim so Swift never calls the varargs builtin.
        let fd = jb_shm_open_rw(name)
        if fd < 0 { throw WriteError.openFailed(errno) }
        defer { close(fd) }

        var st = stat()
        guard fstat(fd, &st) == 0, st.st_size == ShmReader.regionsSize else {
            throw WriteError.wrongSize(Int(st.st_size))
        }
        let map = mmap(nil, ShmReader.regionSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
        guard map != MAP_FAILED else { throw WriteError.mapFailed(errno) }
        defer { munmap(map, ShmReader.regionSize) }
        map!.storeBytes(of: nonce.littleEndian, toByteOffset: 0x1c8, as: UInt64.self)
    }
}
