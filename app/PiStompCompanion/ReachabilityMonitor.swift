import Foundation
import Network

/// Dumb-by-design reachability: TCP-connect to the configured Pi hostname
/// (default `pistomp.local`) with a 1.5 s timeout, then to the last-known-good
/// IP persisted in UserDefaults.
///
/// One honest fact — reachable or not — and which address answered. No
/// ranking, no interface preference, no keepalive theory.
final class ReachabilityMonitor {
    struct Result {
        var reachable = false
        /// The address that answered (hostname or IP string).
        var resolvedAddress: String?
        /// Which name worked: the configured hostname or "cached-ip".
        var via: String?
    }

    /// Called on the monitor's own queue with an immutable result, not on
    /// main: `StatusMonitor` owns the merge and hops to its own queue.
    var onUpdate: ((Result) -> Void)?

    private let queue = DispatchQueue(label: "com.treefallsound.companion.reachability", qos: .utility)
    private var timer: DispatchSourceTimer?
    private var pathMonitor: NWPathMonitor?
    private var inflight = false

    static var cachedIP: String? {
        get { UserDefaults.standard.string(forKey: "lastKnownGoodIP") }
        set { UserDefaults.standard.set(newValue, forKey: "lastKnownGoodIP") }
    }

    func start(interval: TimeInterval = 15.0) {
        stop()
        let path = NWPathMonitor()
        path.pathUpdateHandler = { [weak self] _ in
            self?.probe()
        }
        path.start(queue: queue)
        pathMonitor = path

        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 1, repeating: interval)
        t.setEventHandler { [weak self] in self?.probe() }
        timer = t
        t.resume()
    }

    func stop() {
        pathMonitor?.cancel()
        pathMonitor = nil
        timer?.cancel()
        timer = nil
    }

    private func probe() {
        guard !inflight else { return }
        inflight = true

        let hostname = JackTools.piHostname
        freeTryHost(hostname, timeout: 1.5, queue: queue) { [weak self] ok, addr in
            guard let self else { return }
            if ok {
                self.finish(reachable: true, address: addr ?? hostname, via: hostname)
            } else if let ip = Self.cachedIP, ip != hostname {
                freeTryHost(ip, timeout: 1.5, queue: self.queue) { ok2, addr2 in
                    self.finish(reachable: ok2, address: ok2 ? (addr2 ?? ip) : nil, via: ok2 ? "cached-ip" : nil)
                }
            } else {
                self.finish(reachable: false, address: nil, via: nil)
            }
        }
    }

    private func finish(reachable: Bool, address: String?, via: String?) {
        var r = Result()
        r.reachable = reachable
        r.resolvedAddress = address
        r.via = via
        if reachable, let address, let ip = ipAddress(from: address) {
            Self.cachedIP = ip
        }
        inflight = false
        onUpdate?(r)
    }

    private func ipAddress(from s: String) -> String? {
        // IPv4 literal already; IPv6 literals contain ':'; hostnames drop.
        if s.allSatisfy({ $0.isNumber || $0 == "." }), s.contains(".") { return s }
        if s.contains(":") { return s }
        return nil
    }
}

/// TCP connect to `host:22`; reports success and the peer's address.
/// Calls back exactly once, on `queue`.
func freeTryHost(_ host: String, port: UInt16 = 22, timeout: TimeInterval,
             queue: DispatchQueue, completion: @escaping (Bool, String?) -> Void) {
    let conn = NWConnection(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port)!, using: .tcp)
    var done = false
    let finish: (Bool, String?) -> Void = { ok, addr in
        queue.async {
            guard !done else { return }
            done = true
            conn.cancel()
            completion(ok, addr)
        }
    }

    conn.stateUpdateHandler = { state in
        switch state {
        case .ready:
            finish(true, endpointHost(conn.currentPath?.remoteEndpoint))
        case .failed, .cancelled:
            finish(false, nil)
        default:
            break
        }
    }
    queue.asyncAfter(deadline: .now() + timeout) { finish(false, nil) }
    conn.start(queue: queue)
}

/// Extract the host without dropping an IPv6 address after its first colon.
/// Keep IPv6 unbracketed here; URLComponents adds brackets for HTTP URLs.
private func endpointHost(_ endpoint: NWEndpoint?) -> String? {
    guard let endpoint, case .hostPort(let host, _) = endpoint else { return nil }
    switch host {
    case .ipv4(let address):
        return String(describing: address).components(separatedBy: "%").first
    case .ipv6(let address):
        return String(describing: address)
    case .name(let name, _):
        return name
    @unknown default:
        return nil
    }
}

/// Synchronous TCP probe for the diagnostics dump (runs on a utility queue).
func tcpProbeSync(_ host: String, port: UInt16, timeout: TimeInterval = 3.0) -> String {
    let sem = DispatchSemaphore(value: 0)
    let q = DispatchQueue(label: "com.treefallsound.companion.probe")
    var outcome = "timeout"
    q.async {
        freeTryHost(host, port: port, timeout: timeout, queue: q) { ok, addr in
            outcome = ok ? "open (\(addr ?? host))" : "closed/unreachable"
            sem.signal()
        }
    }
    _ = sem.wait(timeout: .now() + timeout + 1)
    return outcome
}
