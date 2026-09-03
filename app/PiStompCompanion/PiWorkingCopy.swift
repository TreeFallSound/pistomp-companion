import Foundation

/// A read-only look at the pi's `~/pi-stomp` working copy, taken immediately
/// before a deploy so the confirmation can name what the deploy destroys.
///
/// The deploy's `git clean -fdx` and `git checkout --detach --force` are
/// unconditional, so without asking the pi first there is no way to tell a
/// deploy that costs nothing from one that discards a day of work. The old
/// alert warned about both in the same words, which is the same as warning
/// about neither: a warning that fires every time carries no information and
/// gets clicked through. A surveyed-clean tree now gets no warning at all,
/// and a dirty one gets the list.
///
/// Read-only is load-bearing. This runs before the user has confirmed
/// anything, so it may not expand the repository, fetch, or write to the pi
/// in any way — a probe that mutates the thing it is reporting on would make
/// the confirmation a lie.
enum PiWorkingCopy {

    /// The one exclusion the deploy's `git clean` carries. Both scripts must
    /// name the same path or the survey reports removals the deploy will not
    /// make, so it is written once here and interpolated into both.
    static let cleanExclusion = ".git-meta/"

    /// How many paths the pi sends per section. The alert shows fewer still;
    /// this cap only keeps a tree full of build output from shipping
    /// thousands of lines back over ssh. Counts are computed on the pi before
    /// the cut, so they stay exact.
    private static let listCap = 20

    struct Survey {
        /// Porcelain lines for tracked files with staged or unstaged edits —
        /// what `checkout --force` discards. Capped for transport.
        var trackedChanges: [String] = []
        /// Paths `git clean -fdx -e <cleanExclusion>` deletes: untracked and
        /// ignored alike. Capped for transport.
        var removals: [String] = []
        /// Full counts, taken before the cap. `trackedChanges.count` is not
        /// the same number and must never be shown as one.
        var trackedChangeCount = 0
        var removalCount = 0
        /// Short SHA of the current HEAD, and the branch it sits on — nil
        /// when the tree is already detached, which is where the last deploy
        /// left it. The deploy detaches again, so this is the only record of
        /// where the tree was.
        var head: String?
        var branch: String?

        /// True only when the pi answered and had nothing to lose.
        var isClean: Bool { trackedChangeCount == 0 && removalCount == 0 }
    }

    enum Outcome {
        /// The pi answered. The survey is complete and its counts are exact.
        case surveyed(Survey)
        /// `~/pi-stomp` exists but carries no `.git`. The deploy expands it
        /// first, and until it does there is no commit to diff against — so
        /// the tree is unsurveyed, which is not the same as clean.
        case unexpanded
        /// The probe did not complete: ssh, timeout, a missing directory, an
        /// unreadable repository. Carries a line fit to show the user.
        case unreadable(String)
    }

    /// Blocks for at most the ssh budget. Never call this on the main queue.
    static func survey(hostname: String = JackTools.piHostname) -> Outcome {
        let ssh = ProcessInfo.processInfo.environment["JACKBRIDGE_SSH_PATH"] ?? "/usr/bin/ssh"
        let result = ProcessRunner.run(ssh, args: [
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=5",
            "-o", "ConnectionAttempts=1",
            "pistomp@\(hostname)",
            // ssh joins the remaining words with spaces and hands the result
            // to the login shell, so the script survives exactly one round of
            // remote quoting. Quote it as a single word and let `sh -c` see
            // it whole.
            "/bin/sh", "-c", shellQuote(remoteScript),
        ], env: JackTools.environment, timeout: 20)

        guard result.status == 0, !result.timedOut else {
            if result.timedOut { return .unreadable("the check timed out") }
            let detail = result.combined
                .split(whereSeparator: \.isNewline)
                .last
                .map { $0.trimmingCharacters(in: .whitespaces) } ?? ""
            return .unreadable(detail.isEmpty ? "the pi did not answer" : detail)
        }
        return parse(result.stdout)
    }

    // MARK: - the pi side

    /// Emits a keyed line protocol rather than anything Swift has to parse
    /// out of human-oriented git output. Every path ends the same way — one
    /// `state=` line — so a half-finished probe cannot read as a clean tree.
    ///
    /// `git status --untracked-files=no` and `git clean -ndx` are disjoint by
    /// construction: the first reports only tracked files, the second only
    /// files git would delete. Nothing is counted twice.
    private static var remoteScript: String {
        """
        set -u
        SRC="$HOME/pi-stomp"
        if [ ! -d "$SRC" ]; then echo state=missing; exit 0; fi
        if [ ! -d "$SRC/.git" ]; then echo state=unexpanded; exit 0; fi
        cd "$SRC" || { echo state=error; exit 0; }
        git rev-parse --git-dir >/dev/null 2>&1 || { echo state=error; exit 0; }
        tracked=$(git status --porcelain --untracked-files=no 2>/dev/null)
        removed=$(git clean -ndx -e \(cleanExclusion) 2>/dev/null | sed 's/^Would remove //')
        echo state=ok
        echo head=$(git rev-parse --short HEAD 2>/dev/null)
        echo branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null)
        echo trackedCount=$(printf '%s\\n' "$tracked" | grep -c . )
        echo removedCount=$(printf '%s\\n' "$removed" | grep -c . )
        printf '%s\\n' "$tracked" | grep . | head -n \(listCap) | sed 's/^/t:/'
        printf '%s\\n' "$removed" | grep . | head -n \(listCap) | sed 's/^/r:/'
        exit 0
        """
    }

    private static func parse(_ output: String) -> Outcome {
        var survey = Survey()
        var state: String?
        for line in output.split(whereSeparator: \.isNewline) {
            let text = String(line)
            if let value = text.dropPrefix("t:") { survey.trackedChanges.append(value); continue }
            if let value = text.dropPrefix("r:") { survey.removals.append(value); continue }
            guard let separator = text.firstIndex(of: "=") else { continue }
            let key = String(text[text.startIndex..<separator])
            let value = String(text[text.index(after: separator)...])
            switch key {
            case "state": state = value
            case "head": survey.head = value.isEmpty ? nil : value
            // A tree the last deploy detached reports "HEAD" as its branch.
            // That is a real answer, not a name — drop it rather than show it.
            case "branch": survey.branch = (value.isEmpty || value == "HEAD") ? nil : value
            case "trackedCount": survey.trackedChangeCount = Int(value) ?? 0
            case "removedCount": survey.removalCount = Int(value) ?? 0
            default: break
            }
        }
        switch state {
        case "ok": return .surveyed(survey)
        case "unexpanded": return .unexpanded
        case "missing": return .unreadable("~/pi-stomp was not found on the pi")
        case "error": return .unreadable("~/pi-stomp is not a readable Git repository")
        // No `state=` at all means the script never ran to its first echo.
        // Treat silence as unknown; never as clean.
        default: return .unreadable("the pi returned no working-copy status")
        }
    }

    private static func shellQuote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}

private extension String {
    func dropPrefix(_ prefix: String) -> String? {
        hasPrefix(prefix) ? String(dropFirst(prefix.count)) : nil
    }
}
