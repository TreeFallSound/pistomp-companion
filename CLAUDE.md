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
just shm          # the shm control fields, raw (read-only, safe while live)
just logs         # os_log stream, subsystem com.treefallsound.companion
just rmshm        # unlink-shm + restart (after a protocol bump)
```

`just shm` is the one to reach for when the link is up, the ports are wired,
and there is still silence. `status` and the menu bar both render an
*interpretation* of the shm state as a single colour; `shm` prints the values
they are interpreting — `driverStatus`, `syncMode`, the daemon heartbeat,
`slavePortsConnected`, the fault bits. A stack that looks entirely healthy and
is silent is usually one field disagreeing with another, and this is how you
see which. Note `syncMode`: it is 1, meaning the daemon owns the timeline and
the driver's `gDevice_*` anchor globals are dead code on that path.

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

### jack2 fork (machine prerequisite)

The engine builds against, and at run time dynamically loads, the
[`TreefallSound/jack2`](https://github.com/TreefallSound/jack2) fork — not
upstream `jackaudio/jack2`. The fork carries the netJACK2 slave reaping fix
(dead slave no longer freezes the audio thread permanently), the KillMaster
use-after-free fix, slave-name deduplication, and the multicast interface
pin. Without them, a pi-Stomp restart or unplug leaves the engine stuck:
each `_HW_GetZeroTimeStamp` waits for the netadapter that no longer exists.

The fork's `build-macos-pkg.sh` installs to `/usr/local` as
`libjack.{dylib,0.1.0.dylib}`. The daemon (`JackBridged`) links there (see
`otool -L`). The HAL driver does not — it is pure CoreAudio.

#### Verifying the installed fork — `just jack-verify`

**Never judge what is installed by file timestamps, and never compare a
binary's mtime against a git commit date.** You build before you commit, so
an artifact that is *older* than the commit is the normal, correct case. This
reasoning produced a confidently wrong "the rebuild never landed" diagnosis
that sent a working machine chasing a non-existent install problem.

Check for the code itself instead:

```sh
just jack-verify     # marker strings + mtimes for the artifacts that matter
```

netJACK2 lives in jackd's **loadable internal clients**, not in `jackd` and
not (for the master) in `libjackserver`. Grepping the wrong file is the
second half of the same mistake:

| Artifact | Source | Carries |
|----------|--------|---------|
| `/usr/local/lib/jack/netmanager.so` | `common/JackNetManager.cpp` | **master**: egress pin, ifindex latch, master dedupe/reaping |
| `/usr/local/lib/jack/netadapter.so` | `common/JackNetAdapter.cpp` | **slave**: reads `JACK_NETJACK_MULTICAST_IF` (runs on the pi, not the Mac) |
| `libjackserver.dylib`, `libjacknet.dylib` | `posix/JackNetUnixSocket.cpp` | socket layer: `SetMulticastIF`, `JoinMCastGroup` |
| `/usr/local/bin/jackd` | — | none of it. Its date proves nothing. |

With `JACK_NETJACK_MULTICAST_IF` set (jackd-launch always sets it), the master
pin takes the env branch and logs **nothing** on success — it logs only on
failure. Silence in the log is the healthy case, not evidence of no pin.

To install the fork on this machine from a side-by-side clone:

```sh
./jack-rebuild-mac.sh    # sudo; bounces the engine
```

The script warns if the fork's working tree is dirty, then builds the current
working tree including uncommitted changes.

For a bundled-jackd layout override at package time:

```sh
just --set jack_prefix /opt/homebrew engine
```

Why the fork exists and how to vendor it: `docs/vendor-jack2.md`.
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

6. **Every shm control field has exactly one writer.** The ownership table
   is at the top of `jackbridge/shared/JackBridge.h`. Writing another
   component's state field is how you get a stack that looks entirely
   healthy — link up, ports wired, packets flowing — and is silent: the
   daemon wrote `driverStatus = INIT` on its way out, and since only
   `_HW_StartIO` ever writes `STARTED` back, its own replacement then took
   the "driver isn't working" early return forever. To signal across the
   boundary, add a request field owned by the sender.

The full list with file/line citations: `docs/idiosyncrasies.md`.
Why the clock-domain rule holds: `docs/CLOCK_WARS.md` and `docs/architecture.md`.

---

## 4b. Who is asking — recovery has two audiences

Every recovery path in this repo exists at two altitudes, and answering at
the wrong one is worse than useless. Decide which question is being asked
before answering it.

**"What does a user do?"** — the answer is the menu bar, and nothing else.
End users have a cable, a pedal, and the Companion's menu. They do not have
a terminal, do not have `just`, do not have ssh to the pi, and have never
heard of netmanager, jackd, or shm. The only recovery verbs that exist for
them are:

| Symptom | User action |
|---------|-------------|
| Ports don't come back after a replug | Nothing — it should self-heal. Wait ~5 s. |
| Pedal turned on (or rebooted) after the Mac | Nothing — the Companion re-asks the pi. Wait ~10 s. |
| Still not green | **Restart JackBridge** |
| DAW silent after any of the above | Re-select the device in the DAW |

That last row is currently unavoidable: a stack bounce flips
`DeviceIsAlive` → 0, and a host that released the device does not re-acquire
it. A cable replug no longer bounces the stack, so it should not reach that
row at all — jackd, the daemon and the master all survive one
(`docs/idiosyncrasies.md`, "Replug recovery on the Mac"). Everything else in that table is a bug if a user ever has to do it after
a plain cable replug — the fork's self-healing is supposed to cover exactly
that. Fix it; do not write it up as a workaround.

Row 2 is `PiSlaveHealer` (`app/PiStompCompanion/PiSlaveHealer.swift`).
**The Mac is the source of truth for the pi's slave, always.** The pi's unit is
not enabled at boot and nothing on the pi starts it; `jackbridge-ctl start`
does, best-effort — and best-effort means it is allowed to silently skip an
unreachable pi, which is why a pedal switched on after the Mac used to sit
yellow forever. The healer makes that skip temporary: stack up, pi reachable,
`slavePortsConnected == 0` for 10 s → `jackbridge-ctl pi-start`, with backoff.
A live daemon heartbeat *is* the intent, so there is no separate flag to keep
in sync, and `stop` ends healing by definition.

`pi-start` is narrow on purpose — `systemctl start` over ssh, nothing else.
Never heal by kicking the agents: that flips `DeviceIsAlive` → 0 and bills the
user the device re-select in row 4 for a repair they never asked for.

**"What do I run to debug this?"** — that is the maintainer altitude:
`just`, `jackbridge-ctl`, ssh, the logs. Use it freely *here*, and never in
an answer about what a user should do.

The pi's ssh login is **`pistomp@pistomp.local`** — key-based, `BatchMode`
works. `cam@` does not exist; it fails with `Permission denied (publickey)`,
which reads like a missing key rather than a wrong user and will send you
chasing the wrong thing. `PI_USER` in `jackbridge/tools/jackbridge-ctl` is
the source of truth; `JACKBRIDGE_PI_HOST` overrides the host.
The systemd unit on the pi is `pi-stomp-jackbridge.service` (system, not
`--user`, so its journal needs `sudo journalctl -u`).

### The netJACK2 loop budget

`/etc/default/jackbridge` on the pi, read by the unit, applied by
`jackbridge-pi-up`:

| Variable | netadapter flag | Default |
|----------|-----------------|---------|
| `JACKBRIDGE_NET_LATENCY` | `-l`, cycles of loop cushion | 2 |
| `JACKBRIDGE_NET_RING` | `-g`, slip-ring frames | 512 |

    sudo systemctl restart pi-stomp-jackbridge      # apply, ~200 ms gap
    .../jackbridge/jackbridge-pi-status             # xruns_1m must be 0

**They are coupled: `NET_RING / 2` must exceed `NET_LATENCY * period`.** The
resampler drives ring occupancy toward the midpoint, so cushion beyond it
overruns — `-l 6 -g 512` xruns the *pi* at ~2/s while every Mac-side metric
stays clean, which reads as "the fix did nothing" rather than as a new fault.
`jackbridge-pi-up` warns to the journal when the inequality fails.

Too small is the other failure: `-l 2` is 2.67 ms against a measured 7.5 ms
worst-case cable RTT (a USB NIC), which xruns the *Mac* — that is the crackle,
not the workgroup or the HAL. Each cycle costs monitoring latency, so use the
smallest pair that holds zero on both sides.

`JB_NET_LATENCY_CYCLES` and `JB_NETADAPTER_RING_FRAMES` in
`jackbridge/shared/JackBridge.h` mirror these for the advertised-latency model
and **do not track them**. Change one, change the other, or the DAW's delay
compensation is wrong by the difference.

The file is device-local — in neither repo, and lost on a reimage.

Never name an internal shell function (`pi_service`, `bootstrap_agent`,
`wired_iface`) as though it were a thing anyone can invoke. They are private
to their scripts. `jackbridge-ctl` subcommands and `just` recipes are real
commands; the functions inside them are not.

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
| Mac-authoritative tuning (plan) | `docs/plan-tuning.md` |
