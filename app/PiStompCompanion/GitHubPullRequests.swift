import Foundation

/// An open pull request that can be deployed directly from its GitHub head ref.
struct GitHubPullRequest: Decodable {
    let number: Int
    let headRefName: String
    let author: Author?

    struct Author: Decodable {
        let login: String
    }

    var menuTitle: String {
        let authorName = author?.login ?? "unknown"
        return "#\(number) \(headRefName) @\(authorName.isEmpty ? "unknown" : authorName)"
    }
}

enum GitHubPullRequests {
    enum LoadError: Error, CustomStringConvertible {
        case unavailable
        case authenticationFailed(String)
        case commandFailed(String)
        case invalidOutput(String)

        var description: String {
            switch self {
            case .unavailable:
                return "GitHub CLI (gh) was not found"
            case .authenticationFailed(let detail):
                return detail.isEmpty ? "GitHub CLI authentication failed; run gh auth login" : detail
            case .commandFailed(let detail):
                return detail.isEmpty ? "gh pr list failed" : detail
            case .invalidOutput(let detail):
                return detail.isEmpty ? "gh returned invalid pull request data" : detail
            }
        }
    }

    private static let executableCandidates = [
        "/opt/homebrew/bin/gh",
        "/usr/local/bin/gh",
        "/usr/bin/gh",
    ]

    static func loadOpen() -> Result<[GitHubPullRequest], LoadError> {
        guard let executable = executableCandidates.first(where: { FileManager.default.isExecutableFile(atPath: $0) }) else {
            return .failure(.unavailable)
        }

        let result = ProcessRunner.run(executable, args: [
            "pr", "list",
            "--repo", "TreeFallSound/pi-stomp",
            "--state", "open",
            "--limit", "100",
            "--json", "number,headRefName,author",
        ], timeout: 15)
        guard result.launchError == nil, !result.timedOut, result.status == 0 else {
            let detail = result.timedOut
                ? "gh pr list timed out"
                : result.launchError ?? result.combined.trimmingCharacters(in: .whitespacesAndNewlines)
            let lower = detail.lowercased()
            if lower.contains("not logged") || lower.contains("authentication") || lower.contains("auth login") {
                return .failure(.authenticationFailed(detail))
            }
            return .failure(.commandFailed(detail))
        }

        do {
            let pullRequests = try JSONDecoder().decode(
                [GitHubPullRequest].self,
                from: Data(result.stdout.utf8)
            )
            return .success(pullRequests)
        } catch {
            return .failure(.invalidOutput(error.localizedDescription))
        }
    }
}
