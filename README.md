# PiStomp Companion

PiStomp Companion is the macOS menu bar app for a JackBridge-backed pi-Stomp
audio interface. It creates a virtual audio device called **JackBridge**:
four inputs and two outputs at 48 kHz. The device is backed by JACK. The menu
bar shows the connection state, the controls, and the diagnostics.

The main use case: a Raspberry Pi runs netJACK2 over Ethernet and acts as a
recording interface for Mac DAWs (Logic, Pro Tools, REAPER).

**Apple Silicon only.** The app runs on arm64. It does not run on Intel Macs.
The `.pkg` files on the [Releases](https://github.com/treefallsound/pistomp-companion/releases)
page are arm64-only and the install fails on x86_64.

## Requirements

- Apple Silicon (arm64) Mac.
- macOS 13 or later.
- Xcode (to build the app).
- JACK2 (for the full stack; the release package installs it).
- For audio: a pi-Stomp device and an Ethernet cable.

## Build the App

The Xcode project is at `app/PiStompCompanion.xcodeproj`.

To build with the command line:

1. Open Terminal.
2. Go to the project folder:

   ```sh
cd pistomp-companion
   ```

3. Build the app:

   ```sh
xcodebuild -project app/PiStompCompanion.xcodeproj -scheme PiStompCompanion -configuration Debug -derivedDataPath build build
   ```

4. Find the app here:

   ```sh
build/Build/Products/Debug/PiStompCompanion.app
   ```

You can also open the project in Xcode and press Command-R (Run). The build
takes about 20 seconds on an M1 Mac.

## Start the App

To start the app, open it:

```sh
open build/Build/Products/Debug/PiStompCompanion.app
```

The app is a menu bar app. It has no Dock icon and it opens no window. Look
for the pi-Stomp icon in the menu bar.

The app reads the shared memory region of the JackBridge stack. It does not
start the stack. The driver, the daemon, and JACK come from the release
packages. If you did not install them, the app shows the offline state. Use
`jackbridge-ctl status` to check the stack.

## Install the Packages

You can use the app without building it. Install the release packages. The
[Releases](https://github.com/treefallsound/pistomp-companion/releases) page
has two packages. Install them in this order:

1. **`jack2-<version>.pkg`** — the JACK2 fork we depend on. Install this
   first. Stock `jackaudio/jack2` 1.9.22 does not have the
   multicast-interface pin. Without this fork, netJACK2 discovery times out
   on hosts with both Wi-Fi and a direct-cable NIC. The package installs to
   `/usr/local`.
2. **`PiStompCompanion-<version>.pkg`** — the app, the HAL driver, the
   `JackBridged` daemon, the LaunchAgents, the route watcher, and the
   `jackd-launch` wrapper. Double-click it and run it. A signed release
   passes Gatekeeper without the unsigned-package workaround that local
   builds need.

You can run the same package again. The postinstall preserves a hand-edited
`config.plist` and only re-bootstraps the LaunchAgents.

## Use the App

One-time preparation:

1. Make sure you are running pistomp 3.3.0 or newer.
2. Install JACK2, then install `PiStompCompanion-<version>.pkg`.

Daily workflow:

1. Connect the Ethernet cable directly from your Mac to the pi.
2. On the pi-Stomp, open the network menu. Select **"Wired Connection"**, then
   enable audio streaming.
3. Ensure the stream state is flowing in the companion menu.
4. Open your DAW. Select **JackBridge** as the audio device.

Command-line escape hatch:

```sh
jackbridge-ctl status
jackbridge-ctl start
```

### The audio channels in your DAW

| DAW input  | Source                                            |
|------------|---------------------------------------------------|
| In1, In2   | Raw hardware capture from the pi (guitar before the pedalboard) |
| ModOut1/2  | The signal after mod-host (the pedalboard tone)   |
| **Out1/2** | The stereo monitor return to the pi               |

### What the menu shows

- **Reachability.** The icon dims when the pi is unplugged.
- **Audio state badge:**
  - Amber: the pi is reachable but not in the JACK graph.
  - Green: linked and streaming.
  - Red: the shared memory protocol does not match.
- **Start, stop, and restart** shortcuts.
- **One-click SSH and MOD-UI.**
- **"Network Diagnostics…"** collects diagnostic probes into
  `~/Library/Logs/JackBridge/` and opens the folder in Console.app. Run it
  when the pi does not connect. Include the log in any bug report.

The app only reads the shared memory region. It never contends with the
daemon.

## Troubleshooting

### 1. The Mac loses Internet

macOS can prefer the Ethernet cable over Wi-Fi. The pi has no Internet
gateway, so the Mac gets stuck trying to use the cable.

Fix: set the service order.

1. Open System Settings > Network.
2. Select the "..." (three dots) menu, then **"Set Service Order…"**.
3. Drag **Wi-Fi** above the Ethernet device (sometimes called "10/100/1000").

### 2. The services do not start (or the ports are wrong)

Cause: MOD Desktop, Jamulus, or SONABUS can own the `default` JACK server.
JackBridge does not stop them, and it cannot load `netmanager` into another
program's server. The menu shows **"A different program uses JACK"** while it
waits.

Fix: stop the other program. JackBridge starts automatically. Use
`jackbridge-ctl restart` to check again immediately.

Stale JackBridge server: if the launcher stopped but its JACK server remains,
the menu shows **"JackBridge waits for JACK"**. Select **"Quit Other Server"**.
JackBridge stops its own marked server and restarts.

Check the logs: `jackbridge-ctl logs`.

### 3. The pi ports do not appear in `jack_lsp`

Cause: the multicast route lands on the wrong interface (Wi-Fi instead of
Ethernet).

Fix: force the route watcher to re-pin the interface.

```sh
sudo launchctl kickstart -k system/com.jackbridge.route
```

### 4. The audio is silent but the ports are visible

1. Check the connections. Run `jack_lsp -c`. Make sure the `pistomp` ports
   are connected to the `JackBridge` ports. If not, check `AutoConnect` in
   `config.plist`.
2. Wait for the sync. netJACK2's resampler can take 5–10 seconds to stabilize
   on a fresh connection.
3. Check the pi side. SSH into the pi (`ssh pistomp@pistomp.local`) and run
   `jack_lsp`. If the pi does not see its own `system` hardware ports, it has
   nothing to send to the Mac.
4. Restart pi-Stomp. Sometimes the internal audio engine (mod-host) needs a
   kick. Toggle the "Ethernet Audio" setting off and on.

### 5. The audio is distorted or has clicks

Cause: xruns. Check the logs (`jackbridge-ctl logs`). If you see
`JackEngine::XRun`, the latency settings are too aggressive.

Fix: increase `PeriodFrames` in `config.plist` (try 128). Do not raise
`JitterFrames` — it defaults to 0, and the multicast-pin path no longer needs
a HAL-side safety lead.

### 6. jackd does not start on Wi-Fi

This is intentional, not a bug. The route daemon only enables jackd when a
wired/direct-cable interface is up. netJACK2 at 48 kHz with four channels
saturates a typical 2.4 GHz link and causes constant xruns. There is no
override.

Fix: plug in an Ethernet cable (or attach a USB Ethernet adapter). jackd
starts within about 2 seconds. See `docs/architecture.md` for the
clock-domain rationale and why we do not add SRC.

## Configuration

The file `/Library/Application Support/JackBridge/config.plist` holds the
settings. Saving it restarts the LaunchAgents (WatchPaths).

- `ClockDeviceUID` — the CoreAudio UID for jackd's backend device. Empty =
  auto-detect the built-in output.
- `PeriodFrames` — the dominant latency control. 64 or 128 is recommended.
- `NetworkInterface` — the name of the NIC. Empty = auto-detect (prefers
  169.254.x).
- `PiHostname` — the hostname for Pi reachability, MOD-UI, SSH, and
  diagnostics. Default: `pistomp.local`.

## Architecture

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

Optional helper tools for developers:

```sh
gcc -O2 jackbridge/tools/rmshm.c -o jackbridge/tools/rmshm
gcc -O2 jackbridge/tools/chkshm.c -o jackbridge/tools/chkshm
```

## License

See `LICENSE`. The JackBridge engine inherits from the upstream
`madhatter68/JackRouter` project.
