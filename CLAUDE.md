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

### The shape of the justfile

Recipes come in four layers. **Each layer does only its own job.**

| Layer | Does | Recipes |
|-------|------|---------|
| build | Compiles into the build tree. Touches nothing else. | `driver`, `daemon`, `engine`, `app` |
| install | Copies into system paths. Needs sudo. | `install-engine`, `install-scripts`, `install` |
| control | Acts on the running stack. | `restart`, `unlink-shm`, `status`, `logs`, `device-name` |
| loop | What you actually type. Composes the layers. | `reload`, `reload-all`, `reload-scripts`, `rmshm` |

One rule makes them composable:

> **install never restarts, and control never installs.**

Chain as many installs as you like, then restart exactly once. Never chain
two recipes that each restart — and not just for speed. See below.

### The one ordering constraint

    install  →  unlink-shm  →  restart

Always in that order. The HAL driver creates the `/JackBridge` shm region in
`_HW_Open`, and `_HW_Open` runs only when coreaudiod loads the plug-in. So the
unlink must land *after* the new binaries are in place and *before* the
restart that loads them.

Unlink after the restart instead and the name stays gone: nothing recreates
it, and `JackBridged` crash-loops on ENOENT until the next coreaudiod bounce.
This is also why chaining two restarting recipes is harmful — the second
`killall coreaudiod` lands after the region was rebuilt.

`restart` itself has two non-obvious steps for the same reason. It kills the
plug-in's host process (`Core Audio Driver (JackBridgePlugIn.driver)`), which
survives a coreaudiod bounce on its own, and it runs `system_profiler
SPAudioDataType` to force a device enumeration — coreaudiod respawns lazily
and will not load HAL plug-ins until something asks for a device.

### The loops

**Engine.** Edit C++ in `jackbridge/`, or any shell helper, then:

```sh
just reload       # build → install → unlink-shm → restart
```

Needs sudo for the copies and the `coreaudiod` kill. Verify with
`just device-name` and `just logs`.

**Shell helpers only.** The helpers in `jackbridge/installer/` ship only in
the `.pkg`, so changing one otherwise costs a full `just pkg-install` — four
xcodebuild runs, one of them a `clean build`, to deliver a text file. Instead:

```sh
just reload-scripts   # install-scripts → restart, no Xcode at all
```

It syntax-checks every helper with `sh -n` before overwriting anything: these
run under launchd, where a syntax error surfaces as a service that will not
stay up rather than as a parse error you can read.

**App.** Edit Swift in `app/`, then:

```sh
just app-restart          # kill the running app, rebuild Debug, relaunch
just app-restart Release  # any xcodebuild configuration
```

This wraps `./app-restart.sh`: `pkill` the old copy, rebuild, relaunch
detached through launchd. Build output is quiet on success and full on
failure; on failure there is no running app, never the old binary pretending
to be new.

**Everything.** `just reload-all` is `reload` plus the app, with the app
quit first (through AppleScript, so `applicationShouldTerminate` runs
`jackbridge-ctl stop`) and relaunched last.

### Individual pieces

```sh
just driver          # HAL driver only
just daemon          # daemon only
just app             # app only, no launch
just install         # install engine + helpers, no restart
just restart         # bounce the stack, no build, no install
just unlink-shm      # drop the shm region, no restart
just run-app         # build the app and open the build-tree copy
```

### Observe and recover

```sh
just device-name  # the CoreAudio device entry (name, channels, transport)
just status       # LaunchAgent health
just logs         # os_log stream, subsystem com.treefallsound.companion
just rmshm        # unlink-shm + restart (after a protocol bump)
```

### What the loops do *not* cover

The `.pkg` generates or templates four things, and no loop recipe touches
them, because a raw copy would write the wrong content:

- `jack-prefix` and `config.plist` — stamped with `JACK_PREFIX` at package time
- the two LaunchAgent plists and the LaunchDaemon plist — they carry a
  per-user path

Change any of those and you need `just pkg-install`.

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

`just reload` copies binaries and helpers directly. For a real installer:

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
