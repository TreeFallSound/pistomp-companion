# PiStomp Companion — open work

The original JackBridge revival plan (Phases 1–4) is complete and shipped as
`v0.2.0`. This repository now owns the PiStomp Companion app and the
JackBridge engine it installs.

See [`docs/releases.md`](../docs/releases.md) for release history and
[`docs/architecture.md`](../docs/architecture.md) for the design.

This file is now the **action list** — only items still owed. Already-
shipped work is intentionally absent; git history is the archive.

## Architectural assumptions (unchanged from the original plan)

- Fork of `madhatter68/JackRouter`. Two-process design (daemon + HAL) via
  POSIX shm. **Config B clock topology**: jackd on the Mac uses the
  CoreAudio backend pinned to a stable hardware device; netJACK2 handles
  Pi↔Mac clock crossing itself. **No SRC in JackBridge.** Scope: 4-in /
  2-out @ 48 kHz.

## Owed

### 1. Plist config actually consumed — `3.4.1` + `3.4.2` + `3.4.3`
**~1 day, three changes that belong together.**

The plist is staged, has working `WatchPaths` on it, and is documented
inline. The daemon and `jackd-launch` together read three keys
(`ClockDeviceUID`, `PeriodFrames`, `NetworkInterface`). The other six
documented keys are dead:

- **3.4.1** — `jackbridge/shared/jb_config.{hpp,cpp}` reader shim. Single source of
  truth for parsing. C++ side via `CFPreferencesCopyAppValue` /
  `CFPropertyListCreateWithData`; shell side via `PlistBuddy` with
  `2>/dev/null || true` fallbacks. Missing file = all defaults. Missing
  key = that key's default. Malformed = log loudly, fall back, do
  **not** refuse to start.
- **3.4.2** — `jackd-launch` consumes `SampleRate`, `RealtimePriority`,
  `NetJack:MTU` in addition to the three already wired.
  `RealtimePriority < 75` logs a warning citing the spike-B clock
  stability result. MTU passes through to `jack_load netmanager -i
  "--mtu <n>"`.
- **3.4.3** — Daemon consumes `AutoConnect.{ToNetmanager,FromNetmanager,
  LocalMonitoring}` (post-`jack_activate` wiring pass that survives
  netmanager reloads) and `Logging.Level` (maps to `jb_log` threshold).

### 2. Uninstall path — `3.9`
**~½ day. Embarrassing that it doesn't exist yet.**

- `jackbridge-ctl uninstall` subcommand: `bootout` both agents in the
  active GUI session; remove LaunchAgents, HAL bundle, support dir, and
  `/usr/local/bin/jackbridge-ctl` symlink; run `jackbridge/tools/rmshm` to
  unlink the POSIX shm; `killall coreaudiod`; `pkgutil --forget
  com.treefallsound.companion`; clear `launchctl disable` state for both labels.
- Ship a double-clickable `Uninstall.command` inside the `.pkg` payload
  (or alongside the README) wrapping the same logic with `sudo` via
  `osascript`.
- Document the manual fallback in `docs/macos-setup.md`.

### 3. Soak test — `2.8`
**~1 day, gates a "release" label on future `.pkg` builds.**

24-hour continuous run on real hardware (Pi → Mac, music playing through
the DAW). Zero clicks, zero hangs. Kill jackd mid-run, confirm the DAW
disconnects cleanly within one cycle. Restart jackd, confirm the daemon
reattaches without manual intervention. Has not been run since the
v0.2.0 fork dependency landed; v0.1.x is the last data point.

### 4. First-run TCC + permissions — `3.7`
**~½ day, only matters if Apple changed TCC behavior in Tahoe.**

Verify whether the HAL needs microphone TCC on first install (it
shouldn't — we don't open an input stream — but I haven't actually
checked). If yes: installer prompts + `docs/macos-setup.md` writeup
of "I have to do what in System Settings?". Defer until someone
actually hits the prompt.

### 5. Broader return-code audit — `2.7`
**~½ day, low priority.**

The `jack_client_open` failure is now loud (the only PARTIAL left in
2.7). The rest — every `AudioObjectGetPropertyData` / `jack_port_*`
return-code path — is unchecked. Skip until you're touching that code
again; the only failure mode is a silent no-op, not a crash.

## Not owed, but worth a follow-up

- **`SLOW-STARTUP.md` (option A)**: 5-line in-process retry loop in the
  daemon's `JackClient` constructor. `jack_client_open` every 250ms for
  ~30s, no process exit, no `ThrottleInterval=10s` penalty. Cuts
  cold-start from "1–2 throttle windows" to "however long jackd takes".
  The 10s throttle fires on every reboot and on every cable replug
  (the route daemon `kickstart -k`s the agents on every iface change),
  so this is a user-visible delay, not a theoretical one.

- **jack2 fork forward-port**: the fork is 4 commits ahead of
  upstream `v1.9.22-waf`. If a future pi-stomp / JackRouter change
  needs a newer jack2 base, the fork needs to rebase. Track via the
  commit-count past `v1.9.22` in `sastraxi/jack2/ChangeLog.rst` —
  nothing in this repo needs to change unless that drift crosses a
  breaking change in upstream's WAF or driver structure.

## Riskiest unknowns (unchanged from original plan)

1. **`coreaudiod` plug-in validation on current macOS.** Spike A
   de-risked on Sequoia 15.7.2; Tahoe with a Developer ID is the next
   data point (a notarized submission hasn't been run end-to-end
   yet — `3.6` is "DONE pending end-to-end run with real secrets").
2. **Config B clock stability.** Spike B de-risked 30s; the 1hr
   capture is still owed (`2.8`'s soak test).
3. **Notarization of a binary that `dlopen`s libjack.** The
   `disable-library-validation` entitlement is in place but unproven
   end-to-end. Submit a dummy `.pkg` once a Developer ID cert is
   available.

## What this plan deliberately does not include (unchanged)

- Higher channel counts (stays 4-in / 2-out). Higher sample rates (stays
  48 kHz). JACK-graph patchability (CoreAudio apps visible as JACK ports).
  Multi-instance. Windows / Linux. Preferences pane. None of these serve the
  pi-Stomp use case; revisit only if asked.
