# PiStomp Companion

PiStomp Companion is a macOS menu bar app. It gives your Mac a virtual audio
device backed by a pi-Stomp pedal over Ethernet. The device is four inputs and
two outputs at 48 kHz. Its name is **pi-Stomp (<host>)**.

Use it to record and monitor through a Mac DAW (Logic, Pro Tools, REAPER).

**Apple Silicon only.** The app and the release packages are arm64-only.
They do not run on Intel Macs.

---

## Requirements

- Apple Silicon Mac, macOS 13 or later.
- A pi-Stomp device (pistomp 3.3.0 or newer).
- An Ethernet cable, connected directly from the Mac to the pi.
- For daily use: the two release packages. For development: Xcode.

---

## Daily use

1. Connect the Ethernet cable directly from the Mac to the pi.
2. On the pi, open the network menu. Select **Wired Connection**, then enable
   audio streaming.
3. Install the packages, then open the app (see below).
4. In your DAW, select **pi-Stomp (<host>)** as the audio device.

The app has no Dock icon and no window. Find the pi-Stomp icon in the menu
bar. The icon shows the connection state. The menu has start, stop, restart,
SSH, MOD-UI, and diagnostics.

### Audio channels in your DAW

| DAW channel | Source |
|-------------|--------|
| In1, In2    | Hardware capture from the pi, before the pedalboard |
| ModOut1/2   | The signal after the pedalboard |
| Out1/2      | The stereo monitor return to the pi |

---

## Install

The [Releases](https://github.com/treefallsound/pistomp-companion/releases)
page has two packages. Install them in this order:

1. **`jack2-<version>.pkg`** — the JACK2 fork the engine needs. Install this
   first.
2. **`PiStompCompanion-<version>.pkg`** — the app, the HAL driver, the daemon,
   and the service scripts.

You can run the same package again to reinstall; your changes `config.plist`
will not be overwritten, but make sure to read the release notes for new
configuration options and defaults.

The app owns the service lifecycle. Opening the app starts the audio stack.
Quitting the app stops it.

---

## Troubleshooting

Ordered by how often each one occurs.

**The Mac loses Internet.**
macOS can prefer the Ethernet cable over Wi-Fi. The pi has no gateway.
Fix: System Settings → Network → "…" → **Set Service Order**. Drag **Wi-Fi**
above the Ethernet device.

**The services do not start.**
MOD Desktop, Jamulus, or SONABUS can own the JACK server.
Fix: stop the other program. The engine starts by itself. The menu shows the
state.

**The pi ports do not appear.**
The network route landed on Wi-Fi, not Ethernet.
Fix: `sudo launchctl kickstart -k system/com.treefallsound.companion.route`.

**The audio has clicks or distortion.**
This is buffer overruns (xruns).
Fix: increase `PeriodFrames` in `config.plist` (try 128).

**No audio over Wi-Fi.**
This is by design, not a bug. Audio needs the Ethernet cable.
Fix: connect the cable, or attach a USB Ethernet adapter.

Full diagnostics and rare cases: `docs/macos-setup.md`.

---

## Development

The command runner is [`just`](https://just.systems). Install it with
`brew install just`. Run `just --list` to see every command.

```sh
# Architecture

JackBridge is the engine inside PiStomp Companion. It is a fork of
[`madhatter68/JackRouter`](https://github.com/madhatter68/JackRouter),
modernized for Apple Silicon and customized for pi-Stomp.

Two processes use one POSIX shared memory region (`/JackBridge`), with atomic
sync:

- The **driver** (HAL plugin) copies audio between the DAW and the shared
  memory.
- The **daemon** (JACK client) copies audio between JACK and the shared
  memory.

There is no SRC inside JackBridge. netJACK2 handles the clock-domain
crossing.

- [docs/architecture.md](docs/architecture.md) — detailed design
- [docs/macos-setup.md](docs/macos-setup.md) — edge cases
- [jackbridge/tools/jackbridge-ctl](jackbridge/tools/jackbridge-ctl) —
  status/stop/start script
- [jackbridge/installer/build-pkg.sh](jackbridge/installer/build-pkg.sh) —
  build the packages

just              # rebuild the engine and reload it live
just run-app      # build and run the app
just app-restart  # kill, rebuild, and relaunch the app
just device-name  # show the audio device entry
just status       # show service health
just logs         # follow the engine logs
```

For a release package: `just pkg-install` (requires sudo).

The user-facing install, diagnostics, and troubleshooting reference is in
`docs/macos-setup.md`. The architecture is in `docs/architecture.md`.

---

## License

See `LICENSE`. The engine is a fork of
[`madhatter68/JackRouter`](https://github.com/madhatter68/JackRouter).
