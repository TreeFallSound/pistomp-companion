# PiStomp Companion Development Guide

PiStomp Companion is the macOS front end for a JackBridge-backed pi-Stomp
audio interface. The app presents status and controls; JackBridge supplies
the virtual 4-in / 2-out CoreAudio device and JACK bridge underneath.

The primary use case is a Raspberry Pi running JACK + netJACK2 as a network
audio interface for macOS DAWs. See `README.md` for the user-facing overview,
`jackbridge/pi/README.md` for the pi side, and `docs/` for architecture and
gotchas.

## Repo layout

```
app/                  PiStompCompanion menu-bar app and Xcode project
jackbridge/           JackBridge engine and deployment subtree
  daemon/             JACK client + shm publisher
  driver/             AudioServerPlugIn HAL bundle + Xcode project
  installer/          build-pkg.sh, LaunchAgents, helpers, postinstall
  pi/                 systemd service + helpers for the pi-Stomp side
  shared/             JackBridge.h IPC contract and logging
  tools/              shm utilities, jackbridge-ctl, diagnostics
docs/                 Architecture, setup, idiosyncrasies, spike results
plans/                Open work and historical planning
```

Source of truth lives in `jackbridge/daemon/`,
`jackbridge/driver/JackBridge/Plug-In/`, and `jackbridge/shared/`.

## Build

Driver and daemon are targets in
`jackbridge/driver/JackBridgePlugIn.xcodeproj`. The full release pipeline:

```bash
# 1. Build the jack2 fork .pkg (one-time, only when the fork moves)
git clone https://github.com/sastraxi/jack2.git
cd jack2 && ./build-macos-pkg.sh 1.9.22-sastraxi.5
#  → build/jack2-1.9.22-sastraxi.5.pkg  (~720 KB, installs to /usr/local)

# 2. Install JACK2, then build the PiStomp Companion .pkg
sudo installer -pkg jack2-1.9.22-sastraxi.5.pkg -target /
./jackbridge/installer/build-pkg.sh [version]
# Apple Silicon Homebrew (jack2 already on /opt/homebrew):
JACK_PREFIX=/opt/homebrew ./jackbridge/installer/build-pkg.sh
```

`build-pkg.sh` does a `check_jack` against `$JACK_PREFIX` and refuses to
build without a `libjack.0.dylib` there. Default `$JACK_PREFIX` is `/usr/local`
(where the jack2 package installs).

Signing/notarization gates on `SIGN_APP_IDENTITY`,
`SIGN_INSTALLER_IDENTITY`, and `NOTARY_PROFILE`; unset means an unsigned local
build.

## jack2 dependency

JackBridge is a JACK client (the daemon `dlopen`s `libjack`). It depends on the [`sastraxi/jack2`](https://github.com/sastraxi/jack2) fork, not upstream `jackaudio/jack2`, for three commits on top of v1.9.22 + the WAF backport from 1.9.23:

- `3a2f2488` — netadapter PI controller integrator reset on ringbuffer reset (without this, the resample ratio biases further from true on each cycle; the existing `JackPIControler::OurOfBounds()` had zero call sites in upstream 1.9.22).
- `719d833a` — `IP_ADD_MEMBERSHIP` / `IP_BOUND_IF` pin on the master's multicast group join via `JACK_NETJACK_MULTICAST_IF`. Stock upstream picks `INADDR_ANY`'s default-route interface, which on a Mac with both wifi and a direct-cable link-local is the wifi one — discovery then times out.
- `b3bfc408` — mirror pin on the slave's outgoing multicast `sendto()`. Without this, the slave's discovery packets leave on the default route (wifi), even when the netJACK2 link is on eth0.

The fork's `build-macos-pkg.sh` produces a `.pkg` that drops everything into `/usr/local`. See `sastraxi/jack2/ChangeLog.rst` and `sastraxi/jack2/build-macos-pkg.sh` for the full list and build details.

## Pi-stomp

We can connect to a Raspberry Pi 5-based guitar pedal called pi-Stomp via ssh:

```bash
ssh pistomp@pistomp.local
```

This connection is always over wifi; it does not use the ethernet connection we stream audio on.

## Architecture in 30 seconds

```
JACK process callback                CoreAudio IO proc
        │                                    │
        ▼                                    ▼
   daemon writes/reads ──── shm ────► driver memcpy in/out
        │                                    │
        ▼                                    ▼
   jackd / netJACK2                  DAW / system audio
```

Two processes, one shared-memory region (`/JackBridge`), two ring buffers (in + out). Both sides run in the same CoreAudio host-clock domain — see `docs/architecture.md` for why and what that constrains.

## Key idiosyncrasies (do not be surprised by these)

- **IPC contract header** lives at `jackbridge/shared/JackBridge.h`. Both targets pick it up via `HEADER_SEARCH_PATHS=$(SRCROOT)/../shared`. Bump `JACKBRIDGE_PROTOCOL_VERSION` on every shm layout change — the refuse-on-mismatch handshake will then force a clean rebuild on both sides.
- **shm sync uses `std::atomic<uint64_t>` with explicit acquire/release.** `static_assert`s in `jackbridge/shared/JackBridge.h` pin size, alignment, and `is_always_lock_free`. Don't reintroduce `volatile`-as-synchronization.
- **Hardcoded `*2` and `8`-byte-per-frame literals** throughout assume stereo float per ring. Don't generalize without auditing every site.
- **`jackbridge/tools/rmshm.c` also unlinks legacy `/jackrouter` + `/jackrouter2` names** — intentional, helps users migrating from the upstream project.
- **HAL flips `kAudioDevicePropertyDeviceIsAlive=0`** when the daemon's heartbeat (`shmDaemonAlive`) stalls past 5 ring-buffer cycles, then re-arms on `_HW_StartIO`. Don't paper over this with stale-buffer playback.
- **The netJACK2 multicast pin lives in the jack2 fork, not JackBridge.** Master + slave both call `setsockopt` to pin the multicast group to a specific interface. The Mac wrapper is `jackbridge/installer/jackd-launch`; the pi helper is `jackbridge/pi/bin/jackbridge-pi-up`.
- **Wifi-router proxy-ARP poisons the pi's link-local unicast.** Even with the multicast group correctly pinned, the Mac's kernel sends ARP requests for the pi's link-local IP out every interface matching 169.254/16 (en7 + en0). The wifi router proxy-ARPs a reply with its own MAC on en0, which often arrives before the pi's real reply on en7, poisoning the cache. The fix is an interface-scoped host route (`route add -host <pi-ip> -interface <iface>`) that restricts ARP resolution to the wired interface. Since multicast packets don't trigger ARP learning on XNU, the `jackbridge-route-watcher` LaunchDaemon runs a background `tcpdump` loop on the wired interface to capture the pi's netJACK2 discovery (UDP to 225.3.19.154:19000), extracts the source IP, and pins the host route. See `installer/jackbridge-route-watcher:101-126`.

Full list with file/line citations: `docs/idiosyncrasies.md`.

## Development principles for this fork

- **Same-clock-domain assumption is load-bearing.** Mac jackd must run with the CoreAudio backend pinned to a stable hardware device; netJACK2 handles the Pi↔Mac clock crossing. Do not add SRC to JackBridge — that's netJACK2's job. See `docs/architecture.md`.
- **Realtime safety in audio paths.** No allocation, no syscalls, no logging, no locks in the HAL IO proc or the daemon's JACK process callback. Ring-buffer memcpy only.
- **Apple Silicon only.** Intel Macs are not supported. The Xcode project
  *can* produce a universal binary (`arm64 x86_64`) and the
  `jackbridge-pi-up` shell scripts are arch-agnostic, but the released
  `.pkg` files on the GitHub Releases page are arm64-only — the
  jack2 fork's `build-macos-pkg.sh` doesn't yet `lipo` an x86_64
  build, so the resulting `/usr/local/lib/libjack.dylib` is single-arch
  and the JackBridge xcodebuild is forced to `ARCHS=arm64` to link.
  Revisit if a user actually needs Intel support.
- **Fail loud, not silent.** Refuse to attach on protocol mismatch; refuse to run on the wrong jackd backend; exit on bad `jack_client_open`. LaunchAgent `KeepAlive` + `WatchPaths` on `config.plist` handle restart.
- **Pragmatic over perfect.** The shm IPC layout, single-device assumption, and 4-in/2-out scope are *fine for the use case* — don't grow them speculatively.

## Logging

Both JackBridge targets route through `jackbridge/shared/jb_log.hpp` → `os_log`, subsystem `com.jackbridge`, categories `daemon` / `driver` / `shm` / `jack`. Tail with:

```sh
log stream --predicate 'subsystem == "com.jackbridge"'
```

Format-string literals only; use `%{public}s` when caller-supplied strings need to be visible.

## Codesigning + macOS specifics

Driver and daemon both need hardened runtime; release builds use Developer ID
and notarization inside a `.pkg`. Daemon carries
`com.apple.security.cs.disable-library-validation` in
`jackbridge/daemon/daemon.entitlements` to `dlopen` libjack. Install path is
`/Library/Audio/Plug-Ins/HAL/JackBridgePlugIn.driver`; daemon + helpers live
under `/Library/Application Support/JackBridge/`. Postinstall restarts
`coreaudiod` and bootstraps the services into the active GUI session.

Details: `docs/macos-setup.md`.

## Tunables — what to change and where

Ordered roughly by latency impact (biggest first), with the latency
delta you get per unit of change.

| Symbol | Knob | Where | Default | Impact on latency (frames per unit) |
|--------|------|-------|---------|-------------------------------------|
| G | netadapter ring size (`-g N`) | `jackbridge/pi/bin/jackbridge-pi-up:50` (deployed: `/usr/local/libexec/jackbridge/jackbridge-pi-up`) | `512` (was adaptive) | **0.5** — half a frame steady-state per ring frame; full frame in burst headroom |
| P_pi | Pi JACK period (`-p N`) | `/etc/default/jack` (`JACK_PERIOD`), seeded by `pistomp-arch/files/pistomp.conf:28` | `64` | T_pj scales 1:1, T_alsa scales N_pi:1, T_l scales L:1 — **the largest knob** |
| N_pi | ALSA periods (`-n N`) | `pistomp-arch/files/jackdrc:19` (hardcoded `-n 2`) | `2` | P_pi frames per period — biggest non-G one-shot saving if dropped to 1 (but risky) |
| L | netadapter network latency (`-l N`, cycles, range 0–30) | `jackbridge/pi/bin/jackbridge-pi-up:50` (currently unset → default) | `2` (jack2 1.9.22, verified on-device) | P_pi frames per cycle |
| P_mac | Mac JACK period (`PeriodFrames`) | `jackbridge/installer/config.plist:44` → `/Library/Application Support/JackBridge/config.plist` | `64` | T_mj scales 1:1; **must match P_pi or netJACK2 resampler chokes** |
| J | HAL safety lead (`JitterFrames`) | `jackbridge/installer/config.plist:52` | `0` | 1:1 — pure latency, no slip-ring effect (single clock domain). Default is 0; the upstream-recommended 192 was rolled back once the new setsockopt-based multicast pin eliminated the need for a HAL-side safety lead. |
| f_s | Sample rate | `pistomp.conf:27` AND `jackbridge/installer/config.plist:30` | `48000` | All times are `frames / f_s`, so doubling f_s halves all ms costs but doubles CPU |
| Q | netadapter resampler quality (`-q N`, **0 = lowest, 4 = highest**) | `jackbridge/pi/bin/jackbridge-pi-up:50` | `0` (we set it explicitly) | No latency impact — only CPU/fidelity |
| MTU | netJACK MTU | `jackbridge/installer/config.plist:64` | `1500` | Affects T_wire only at jumbo-frame scale; only changes packet count, not buffer math |
| RT prio | jackd realtime priority | Pi: hardcoded `-P 75` in `jackdrc:19`. Mac: `RealtimePriority` in `jackbridge/installer/config.plist:58` | `75` both | No direct latency; affects jitter (variance), not mean |
| Storm threshold | Auto-restart on xrun storm | `JACKBRIDGE_XRUN_THRESHOLD` env (read by `jackbridge-xrun-watcher`) | `50/s` | Recovers from degraded state; doesn't change steady-state latency |
| Multicast pin | `JACK_NETJACK_MULTICAST_IF` env var | Read by `JackNetMasterManager` (master) and `JackNetAdapter` (slave) in the jack2 fork. Pi: `jackbridge/pi/bin/jackbridge-pi-up` exports it (default `eth0`, override via `/run/jackbridge.iface` from `jackbridge-pin-route`'s auto-detect). Mac: `jackd-launch` exports it from `/var/run/jackbridge-route.iface`. | empty (legacy INADDR_ANY behavior — broken on multi-NIC hosts) | n/a — required for discovery to work at all when the host has wifi + a direct cable NIC |
