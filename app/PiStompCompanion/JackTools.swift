import Foundation

/// Single source of truth for where the JACK CLI tools live at runtime.
///
/// The package is built against one prefix (`JACK_PREFIX` in
/// `jackbridge/installer/build-pkg.sh`, default `/usr/local`) and the daemon
/// links `libjack` from it, so the Companion must not go probing for some
/// *other* jack — a `/opt/homebrew/bin/jack_lsp` talking to a `/usr/local`
/// jackd is a version-skew bug waiting to happen. build-pkg.sh stamps the
/// prefix it used into `config.plist` as `JackPrefix`; the shell side reads
/// the same key via `jackbridge/installer/jack-prefix.sh`.
///
/// Resolution order (identical in both implementations):
///   1. `JACKBRIDGE_JACK_PREFIX` in the environment — debugging escape hatch.
///   2. `JackPrefix` in config.plist — what the installed package was built
///      against.
///   3. First of `/usr/local`, `/opt/homebrew` that actually has `bin/jackd`
///      — covers installs that predate the key.
///   4. `/usr/local` — the documented default, even if nothing is there yet,
///      so error messages name a real path.
enum JackTools {
    static let defaultPrefix = "/usr/local"
    static let candidatePrefixes = ["/usr/local", "/opt/homebrew"]
    static let configPath = "/Library/Application Support/JackBridge/config.plist"
    static let defaultPiHostname = "pistomp.local"
    /// Read at use time so editing config.plist takes effect without relaunching
    /// the Companion; the LaunchAgents independently restart via WatchPaths.
    static var piHostname: String { configuredPiHostname() ?? defaultPiHostname }
    private static func configuredPiHostname() -> String? {
        guard let data = FileManager.default.contents(atPath: configPath),
              let plist = try? PropertyListSerialization.propertyList(
                  from: data, options: [], format: nil) as? [String: Any],
              let value = plist["PiHostname"] as? String else {
            return nil
        }
        let host = value.trimmingCharacters(in: .whitespacesAndNewlines)
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: ".-_:%"))
        guard !host.isEmpty, host.count <= 253,
              host.rangeOfCharacter(from: allowed.inverted) == nil else {
            return nil
        }
        return host
    }

    /// Resolved once per launch: the prefix can only change via a reinstall,
    /// which restarts the app's world anyway.
    static let prefix: String = resolved.path
    /// How `prefix` was arrived at, for the diagnostics dump.
    static let prefixOrigin: String = resolved.origin

    static var jackd: String { "\(prefix)/bin/jackd" }
    static var jackLsp: String { "\(prefix)/bin/jack_lsp" }

    /// Environment for any jack_* invocation. `JACK_NO_START_SERVER` is
    /// mandatory: without it the tools auto-spawn a stray default jackd (see
    /// JackGraphMonitor and jackbridge/installer/jackd-launch).
    static var environment: [String: String] {
        var env = ProcessInfo.processInfo.environment
        env["JACK_NO_START_SERVER"] = "1"
        return env
    }

    private static let resolved: (path: String, origin: String) = {
        let env = ProcessInfo.processInfo.environment["JACKBRIDGE_JACK_PREFIX"]
        if let env, !env.isEmpty {
            return (env, "JACKBRIDGE_JACK_PREFIX")
        }
        if let configured = configuredPrefix(), !configured.isEmpty {
            return (configured, "config.plist JackPrefix")
        }
        for candidate in candidatePrefixes
        where FileManager.default.isExecutableFile(atPath: "\(candidate)/bin/jackd") {
            return (candidate, "probed")
        }
        return (defaultPrefix, "default")
    }()

    private static func configuredPrefix() -> String? {
        guard let data = FileManager.default.contents(atPath: configPath),
              let plist = try? PropertyListSerialization.propertyList(
                  from: data, options: [], format: nil) as? [String: Any] else {
            return nil
        }
        guard let value = plist["JackPrefix"] as? String else { return nil }
        // The build stamps a literal path; a placeholder means an unstamped
        // template got installed by hand. Ignore it rather than exec'ing it.
        return value.hasPrefix("/") ? value : nil
    }
}
