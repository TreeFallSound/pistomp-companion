import Foundation

/// Single source of truth for where the JACK CLI tools live at runtime.
/// The package stamps its build prefix into installed runtime data rather
/// than exposing that build identity in the user configuration.
enum JackTools {
    static let defaultPrefix = "/usr/local"
    static let candidatePrefixes = ["/usr/local", "/opt/homebrew"]
    static let runtimeSupport = "/Library/Application Support/JackBridge"
    static let configPath = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/JackBridge/config.plist").path
    static let defaultPiHostname = "pistomp.local"
    static var piHostname: String { configuredPiHostname() ?? defaultPiHostname }
    private static func configuredPiHostname() -> String? {
        guard let data = FileManager.default.contents(atPath: configPath),
              let plist = try? PropertyListSerialization.propertyList(
                  from: data, options: [], format: nil) as? [String: Any],
              let value = plist["PiHostname"] as? String else { return nil }
        let host = value.trimmingCharacters(in: .whitespacesAndNewlines)
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: ".-_:%"))
        guard !host.isEmpty, host.count <= 253,
              host.rangeOfCharacter(from: allowed.inverted) == nil else { return nil }
        return host
    }

    static let prefix: String = resolved.path
    static let prefixOrigin: String = resolved.origin
    static var jackd: String { "\(prefix)/bin/jackd" }
    static var jackLsp: String { "\(prefix)/bin/jack_lsp" }
    static var coordinator: String { "\(runtimeSupport)/jackbridge-coordinator" }
    static var environment: [String: String] {
        var env = ProcessInfo.processInfo.environment
        env["JACK_NO_START_SERVER"] = "1"
        return env
    }

    private static let resolved: (path: String, origin: String) = {
        if let env = ProcessInfo.processInfo.environment["JACKBRIDGE_JACK_PREFIX"], !env.isEmpty {
            return (env, "JACKBRIDGE_JACK_PREFIX")
        }
        let stamped = "\(runtimeSupport)/jack-prefix"
        if let value = try? String(contentsOfFile: stamped, encoding: .utf8) {
            let path = value.trimmingCharacters(in: .whitespacesAndNewlines)
            if path.hasPrefix("/") { return (path, "installed runtime prefix") }
        }
        for candidate in candidatePrefixes
        where FileManager.default.isExecutableFile(atPath: "\(candidate)/bin/jackd") {
            return (candidate, "probed")
        }
        return (defaultPrefix, "default")
    }()
}
