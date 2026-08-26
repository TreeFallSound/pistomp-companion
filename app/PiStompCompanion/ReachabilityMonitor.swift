import Foundation
import Network

/// Dumb-by-design reachability: TCP-connect to sshd (port 22) with a 1.5 s
/// timeout, first on `pistomp.local` (NWConnection does the mDNS
/// resolution), then on the last-known-good IP persisted in UserDefaults.
///
/// One honest fact — reachable or not — and which address answered. No
/// ranking, no interface preference, no keepalive theory.
final class ReachabilityMonitor {
    struct Result {
        var reachable = false
        /// The address that answered (hostname or IP string).
        var resolvedAddress: String?
        /// Which name worked: "pistomp.local" or "cached-ip".
        var via: String?
    }

    private(set) var lastResult = Result()
    var onUpdate: (() -> Void)?

    private let queue = DispatchQueue(label: "com.jackbridge.companion.reachability", qos: .utility)
    private var timer: DispatchSourceTimer?
    private var inflight = false

    static var cachedIP: String? {
        get { UserDefaults.standard.string(forKey: "lastKnownGoodIP") }
        set { UserDefaults.standard.set(newValue, forKey: "lastKnownGoodIP") }
    }

    func start(interval: TimeInterval = 15.0) {
        stop()
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 1, repeating: interval)
        t.setEventHandler { [weak self] in self?.probe() }
        timer = t
        t.resume()
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    private func probe() {
        guard !inflight else { return }
        inflight = true

        freeTryHost("pistomp.local", timeout: 1.5, queue: queue) { [weak self] ok, addr in
            guard let self else { return }
            if ok {
                self.finish(reachable: true, address: addr ?? "pistomp.local", via: "pistomp.local")
            } else if let ip = Self.cachedIP, ip != "pistomp.local" {
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
        lastResult = r
        if reachable, let address, let ip = ipAddress(from: address) {
            Self.cachedIP = ip
        }
        inflight = false
        DispatchQueue.main.async { [weak self] in self?.onUpdate?() }
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
            var addr: String?
            if case .hostPort(let h, _)? = conn.currentPath?.remoteEndpoint,
               case .ipv4(let a) = h {
                addr = "\(a)"
            } else if let ep = conn.currentPath?.remoteEndpoint {
                addr = String(describing: ep).components(separatedBy: ":").first
            }
            finish(true, addr)
        case .failed, .cancelled:
            finish(false, nil)
        default:
            break
        }
    }
    queue.asyncAfter(deadline: .now() + timeout) { finish(false, nil) }
    conn.start(queue: queue)
}

/// Synchronous TCP probe for the diagnostics dump (runs on a utility queue).
func tcpProbeSync(_ host: String, port: UInt16, timeout: TimeInterval = 3.0) -> String {
    let sem = DispatchSemaphore(value: 0)
    let q = DispatchQueue(label: "com.jackbridge.companion.probe")
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
