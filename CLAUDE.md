# PiStomp Companion — Development Guide

PiStomp Companion is the macOS front end for the JackBridge audio engine.
The app shows status and controls. JackBridge supplies the virtual 4-in /
2-out CoreAudio device and the JACK bridge.

Use this file to work on the code. For users, `README.md`. For the design,
`docs/architecture.md`. For the rules that will surprise you,
`docs/idiosyncrasies.md`.

---

## 1. Build and run (most common tasks)

The command runner is [`just`](https://just.systems). Install it with
`brew install just`. Run `just --list` to see every command.

**Engine loop.** Edit C++ in `jackbridge/`, then:

```sh
just reload       # build driver + daemon, swap binaries, bounce the stack
```

This copies the built binaries over `/Library/...` and restarts `coreaudiod`
and the two LaunchAgents. It needs sudo for the copy and the `coreaudiod`
kill. Verify with `just device-name` and `just logs`.

**App loop.** Edit Swift in `app/`, then:

```sh
just app-restart          # kill the running app, rebuild Debug, relaunch
just app-restart Release  # any xcodebuild configuration
```

This wraps `./app-restart.sh`: `pkill` the old copy, rebuild, relaunch
detached through launchd. The relaunched app survives you closing the
terminal. Build output is quiet on success and full on failure; on failure
there is no running app, never the old binary pretending to be new.

**Individual targets** when you want one piece:

```sh
just driver       # HAL driver only
just daemon       # daemon only
just app          # app only, no launch
just run-app      # build the app and open the build-tree copy
```

**Observe:**

```sh
just device-name  # the CoreAudio device entry (name, channels, transport)
just status       # LaunchAgent health
just logs         # os_log stream, subsystem com.treefallsound.companion
```

**Recover:**

```sh
just restart      # bounce the agents and coreaudiod, no rebuild
just rmshm        # drop the stale shm region (needed after a protocol bump)
```

---

## 2. Repo layout

```
app/                  menu-bar app and Xcode project
jackbridge/
  daemon/             JACK client + shm publisher
  driver/             AudioServerPlugIn HAL bundle + Xcode project
  installer/          build-pkg.sh, LaunchAgents, helpers, postinstall
  pi/                 systemd service + helpers for the pi side
  shared/             JackBridge.h IPC contract and logging
  tools/              jackbridge-ctl, rmshm.c
docs/                 architecture, setup, idiosyncrasies, spike results
```

Source of truth: `jackbridge/daemon/`,
`jackbridge/driver/JackBridge/Plug-In/`, `jackbridge/shared/`.

For a guided read: `docs/codebase-tour.md`.

---

## 3. Release builds

`just reload` copies binaries directly. For a real installer:

```sh
just pkg            # build the .pkg (unsigned local build by default)
just pkg-install    # build and install the .pkg (sudo)
```

Signing and notarization gate on `SIGN_APP_IDENTITY`,
`SIGN_INSTALLER_IDENTITY`, and `NOTARY_PROFILE`. Unset means an unsigned
local build.

The engine links against the [`sastraxi/jack2`](https://github.com/sastraxi/jack2)
fork, not upstream `jackaudio/jack2`. The fork's `build-macos-pkg.sh`
installs to `/usr/local`. Override the prefix:

```sh
just --set jack_prefix /opt/homebrew engine
```

Why the fork is required, and how to build it: `docs/vendor-jack2.md`.
How to ship a release: `docs/releases.md`.

---

## 4. Architecture in 30 seconds

```
JACK process callback                CoreAudio IO proc
        │                                    │
        ▼                                    ▼
   daemon writes/reads ──── shm ────► driver memcpy in/out
        │                                    │
        ▼                                    ▼
   jackd / netJACK2                  DAW / system audio
```

Two processes, one POSIX shared-memory region (`/JackBridge`), two ring
buffers. Both run in the same CoreAudio host-clock domain. netJACK2 does the
Pi↔Mac clock crossing. There is no SRC in JackBridge.

Full design and constraints: `docs/architecture.md`.

### The rules you must not break

1. **Bump `JACKBRIDGE_PROTOCOL_VERSION` on every shm layout change.** The
   handshake refuses to attach on mismatch, forcing a clean rebuild on both
   sides. The contract header is `jackbridge/shared/JackBridge.h`.
2. **No allocation, syscalls, logging, or locks in the audio paths.** The
   HAL IO proc and the daemon's JACK process callback do ring-buffer memcpy
   only. This is a realtime constraint.
3. **No SRC in JackBridge.** Both sides share the CoreAudio clock; netJACK2
   handles the cross-clock resampling. This is load-bearing.
4. **Fail loud, not silent.** Refuse to attach on protocol mismatch; refuse
   the wrong jackd backend; exit on a bad `jack_client_open`.
5. **shm sync uses `std::atomic<uint64_t>` with explicit acquire/release.**
   `static_assert`s pin size, alignment, and `is_always_lock_free`. Do not
   reintroduce `volatile`-as-synchronization.

The full list with file/line citations: `docs/idiosyncrasies.md`.
Why the clock-domain rule holds: `docs/CLOCK_WARS.md` and `docs/architecture.md`.

---

## 5. Logging and diagnostics

Both engine targets log through `jackbridge/shared/jb_log.hpp` → `os_log`,
subsystem `com.treefallsound.companion`, categories `daemon` / `driver` /
`shm` / `jack`.

```sh
just logs
```

Use format-string literals only. Use `%{public}s` for caller-supplied
strings.

The menu-bar "Network Diagnostics…" collects probes into
`~/Library/Logs/JackBridge/`. Run it when the pi does not connect, and attach
the log to bug reports.

---

## 6. Platform constraints

- **Apple Silicon only.** The released packages and the default `just`
  builds are arm64. The jack2 fork does not yet build an x86_64 `libjack`,
  so the engine cannot link Intel. Revisit only if a user needs it.
- **Codesigning.** The driver and daemon run with the hardened runtime. The
  daemon carries `com.apple.security.cs.disable-library-validation` in
  `jackbridge/daemon/daemon.entitlements` to `dlopen` libjack.
- **Install paths.** Driver:
  `/Library/Audio/Plug-Ins/HAL/JackBridgePlugIn.driver`. Daemon and helpers:
  `/Library/Application Support/JackBridge/`. Postinstall restarts
  `coreaudiod` and bootstraps the services into the active GUI session.

Codesigning and notarization detail: `docs/macos-setup.md`.

---

## 7. Where to look next

Read these when you touch the matching area, not before:

| Topic | File |
|-------|------|
| Architecture and clock domains | `docs/architecture.md` |
| Latency math and tunables | `docs/LATENCY-MODEL.md` |
| Jitter and crackle | `docs/JITTER.md` |
| Why one clock, the four options | `docs/CLOCK_WARS.md` |
| Surprising behaviors, with citations | `docs/idiosyncrasies.md` |
| Install, config, post-install | `docs/macos-setup.md` |
| The pi side | `docs/pi-stomp.md` |
| The jack2 fork | `docs/vendor-jack2.md` |
| Shipping a release | `docs/releases.md` |
| Walkthrough of the source tree | `docs/codebase-tour.md` |
