# macOS setup

The user-facing runtime setup. What gets installed where, what processes run, how the pieces wire together.

## Components

| Component | Role | Lifecycle |
|---|---|---|
| `JackBridgePlugIn.driver` | AudioServerPlugIn HAL bundle. Presents the CoreAudio device DAWs select. | Loaded by `coreaudiod`. Reloads on `killall coreaudiod`. |
| `JackBridge` daemon | JACK client. Shuttles buffers between jackd and the HAL via shm. | LaunchAgent, per-user session. |
| `jackd` (from JACK2 official) | JACK audio server. Hosts the netJACK2 master client. | LaunchAgent, per-user session. |
| `jack-clock` aggregate device | Provides jackd's CoreAudio backend with a stable clock source. | Created at install time via `AudioHardwareCreateAggregateDevice`. |
| Pi-side `jackd -d net` | netJACK2 slave on the Raspberry Pi. | Pi systemd service. Out of scope for this doc. |

## Install paths

```
/Library/Audio/Plug-Ins/HAL/JackBridgePlugIn.driver    HAL bundle (system-wide)
/Library/Application Support/JackBridge/JackBridged    Daemon binary
/Library/LaunchAgents/com.jackbridge.daemon.plist      Daemon LaunchAgent
/Library/LaunchAgents/com.jackbridge.jackd.plist       jackd launcher
/Library/Application Support/JackBridge/jackd-launch   jackd wrapper script
~/Library/Logs/JackBridge/                              Unified-log mirror (debug)
```

LaunchAgents in `/Library/LaunchAgents/` are instantiated per-user-session. Each plist sets `LimitLoadToSessionType = Aqua` so they only run for GUI sessions (not SSH, not loginwindow). `coreaudiod` is per-session — system-wide LaunchDaemons (uid 0, no Aqua) cannot wire up to it cleanly, which is why we don't use them.

## Clock-device selection

`jackd -d coreaudio` requires a CoreAudio device to drive its cycle. The choice is load-bearing (see `architecture.md` — single-clock-domain) and we want it:

1. **Always present** (so jackd starts reliably without depending on user hardware).
2. **Stable in clock** (so jackd's cycle doesn't jitter).
3. **Independent of whatever device the DAW selects** (no circular dependency).
4. **Independent of what JackBridge presents** (no feedback loop — JackBridge as jackd's backend is the silence/runaway condition documented in `idiosyncrasies.md`).

The built-in audio output meets all four on any Mac that has it. `jackd-launch` reads `ClockDeviceUID` from the user's `~/Library/Application Support/JackBridge/config.plist`; when empty, the `jb-detect-builtin` helper (installed alongside the daemon) enumerates CoreAudio devices, filters for transport-type `BuiltIn` with output streams, and prints the first match's UID. Headless Macs (Mac mini / Studio with no built-in codec) set `ClockDeviceUID` in JackBridge Settings — enumerate UIDs with `system_profiler SPAudioDataType` or `jackd -d coreaudio -l`.

We don't use a CoreAudio aggregate wrapper around the built-in: it would add a phantom "JackBridge Clock" device to every DAW's picker for no clock-stability gain (built-in UIDs are stable across reboots). UID-based lookup via `~:<uid>` is what bypasses Spike B's friendly-name-fallback bug; the indirection wasn't buying us that.

## Bringing the system up (post-install)

```bash
# LaunchAgents start automatically at user login. Manual control:
launchctl load   ~/Library/LaunchAgents/com.jackbridge.jackd.plist
launchctl load   ~/Library/LaunchAgents/com.jackbridge.daemon.plist
launchctl unload ~/Library/LaunchAgents/com.jackbridge.daemon.plist
launchctl unload ~/Library/LaunchAgents/com.jackbridge.jackd.plist
```

The `jackd-launch` wrapper script invokes:
```
jackd -R -P 75 -d coreaudio -d ~:<ClockDeviceUID> -r 48000 -p 128
```
then `jack_load netmanager` once jackd is responsive (netJACK2 master client).

**`-P 75` is required, not optional.** macOS jackd's default realtime priority is `10`, which loses to almost anything on the system; in testing, the netJACK2 master client missed its deadline ~2.3×/sec at the default and the slave connection actually dropped within 30s. `-P 75` (matching the Pi's typical setting) eliminates xruns. See `spike-b-clock-stability.md`.

## Selecting the device in a DAW

1. Install completes, `coreaudiod` is restarted.
2. DAW opens, audio device picker shows "JackBridge" alongside the user's other interfaces.
3. DAW selects JackBridge. 2 input channels, 4 output channels visible.
4. Pi-side jackd discovers the Mac's `netmanager` via multicast (default `225.3.19.154`) and the netJACK2 link comes up.
5. JACK graph routing happens on the Pi side (typically wired by pi-stomp or its host configuration).

## "System audio through the Pi" bonus mode

Setting **System Settings → Sound → Output → JackBridge** routes all system audio (Spotify, browser, video players, system alerts) through the chain → Pi → Pi's audio hardware. This is independent of the DAW use case.

Caveats:
- Latency ~5–15 ms end-to-end. Imperceptible for music, fine for video (players resync), edge-case for real-time interactive (Zoom, games).
- Stereo only. Multichannel system audio is summed/dropped.
- 48 kHz only. CoreAudio resamples app-side transparently.

## Turning JackBridge on and off

The `jackbridge-ctl` wrapper (installed alongside the daemon) handles both LaunchAgents together. Stopping only one leaves either the audio device orphaned or the daemon crash-looping, so always move them as a pair.

```bash
jackbridge-ctl start     # enable + bootstrap both agents into this GUI session
jackbridge-ctl stop      # bootout + disable both (persists across reboots)
jackbridge-ctl restart   # stop + start
jackbridge-ctl status    # per-agent state
jackbridge-ctl logs      # tail -F the agent stdout/stderr logs in /tmp
```

`stop` writes to launchd's disabled-state database (`/var/db/com.apple.xpc.launchd/disabled.<uid>.plist`), so the agents stay off across reboots and login cycles until `jackbridge-ctl start` re-enables them. No `sudo` required — agents run in the user's GUI session.

Raw `launchctl` equivalent (for reference / scripting):
```bash
launchctl disable gui/$(id -u)/com.jackbridge.daemon
launchctl disable gui/$(id -u)/com.jackbridge.jackd
launchctl bootout  gui/$(id -u)/com.jackbridge.daemon
launchctl bootout  gui/$(id -u)/com.jackbridge.jackd
# … and the inverse with `enable` + `bootstrap`.
```

### What happens if the daemon refuses to start?

The daemon validates jackd's CoreAudio backend on startup (see `plans/PLAN.md` history — phase 2.5 + 2.6):
- No `system:playback_1` port, or no aliases on it → jackd's backend isn't `coreaudio`. Refuse.
- Alias contains `"JackBridge"` → jackd is clocked off JackBridge itself, which creates a CoreAudio feedback loop. Refuse.

On either failure the daemon logs to `com.jackbridge` / category `jack` via `os_log` and exits non-zero. `KeepAlive=true` means launchd respawns after its built-in 10s throttle — the daemon will loop until you fix the cause. Watch the loop with:

```bash
log stream --predicate 'subsystem == "com.jackbridge" && category == "jack"'
# or:
jackbridge-ctl logs
```

Use JackBridge Settings to set `ClockDeviceUID`; changing settings atomically replaces the home plist and launchd restarts the existing agents. If the daemon refuses to start, inspect the coordinator and JACK logs. `jackbridge-ctl stop` disables the stack without changing settings.

## Codesigning + notarization

| Artifact | Signature | Hardened runtime | Entitlements | Notarized |
|---|---|---|---|---|
| `JackBridgePlugIn.driver` | Developer ID Application | Yes | None | Yes (inside `.pkg`) |
| `JackBridged` daemon | Developer ID Application | Yes | `com.apple.security.cs.disable-library-validation` (for `libjack.dylib`) | Yes (inside `.pkg`) |
| `JackBridge-x.y.z.pkg` | Developer ID Installer | n/a | n/a | Yes, stapled |

Sequence:
```bash
codesign --force --options runtime --sign "Developer ID Application: <Team>" \
    --entitlements daemon.entitlements JackBridged
codesign --force --options runtime --sign "Developer ID Application: <Team>" \
    --deep JackBridgePlugIn.driver

pkgbuild --root staging --identifier com.jackbridge.pkg --version 0.1.0 \
    --install-location / --scripts scripts JackBridge.pkg
productbuild --sign "Developer ID Installer: <Team>" \
    --package JackBridge.pkg JackBridge-signed.pkg

xcrun notarytool submit JackBridge-signed.pkg \
    --apple-id <id> --team-id <team> --password <app-specific> --wait
xcrun stapler staple JackBridge-signed.pkg
```

Without notarization the driver loads on the dev's machine but Gatekeeper blocks the `.pkg` for end users.

## libjack on the Mac

The daemon dynamic-links `libjack.0.dylib`. JACK2 v1.9.22+ ships a package
that installs to `/usr/local/lib/libjack.0.dylib`. **Do not use Homebrew's
`jack` formula** unless it is the required JACK2 fork; stock JACK1 does not
include netJACK2 in the same form.

The hardened-runtime entitlement is required because libjack is signed by
the JACK project, not by us, and library validation rejects cross-team-signed
libraries by default.

## Troubleshooting

- **"JackBridge doesn't appear in Audio MIDI Setup"** — codesigning issue or non-arm64 build. Check `log show --predicate 'subsystem == "com.apple.audio.coreaudiod"' --last 10m` for rejection reasons.
- **"DAW sees JackBridge but no audio"** — daemon not running, or daemon running but jackd not. `jackbridge-ctl status`.
- **"Clicks/dropouts"** — in order of likelihood: Mac jackd priority, Mac on Wi-Fi, or Pi-side buffer size. See the Companion diagnostics dump.
- **"Stale shm region"** — run `jackbridge/tools/rmshm`, then restart the services.
