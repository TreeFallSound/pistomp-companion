# PiStomp Companion

PiStomp Companion is the macOS front end for a JackBridge-backed pi-Stomp
audio interface. It installs a virtual **JackBridge** audio device (4-in /
2-out @ 48 kHz) backed by JACK and presents the connection state, controls,
and diagnostics from the menu bar.

The primary use case is a Raspberry Pi running netJACK2 over Ethernet as a
recording interface for Mac DAWs (Logic, Pro Tools, REAPER).

**Apple Silicon only** (arm64). Intel Macs are not supported — the `.pkg`
files on the [Releases](https://github.com/treefallsound/pistomp-companion/releases)
page are arm64-only and the install will fail on x86_64.

---

## Daily Workflow (Ongoing Use)

The PiStomp Companion menu-bar app is the normal control surface. It starts
with macOS login when enabled and shows whether the Mac services, Pi, JACK
graph, and audio stream are healthy.

1. **Install:** Install JACK2, then install `PiStompCompanion-<version>.pkg`.
2. **Connect:** Plug the Ethernet cable directly from your Mac to the Pi.
3. **Start Pi Service:** On the pi-Stomp device, go to the network menu, choose
   **"Wired Connection"**, then enable audio streaming.
4. **Confirm Connection:** Open the Companion menu. It should report
   streaming; alternatively run `jack_lsp | grep pistomp`.
5. **DAW Setup:** Open your DAW and select **JackBridge** as the audio device.

The command-line escape hatch remains available:

```sh
jackbridge-ctl status
jackbridge-ctl start
```

### What you get in the DAW

| DAW input  | Source                                            |
|------------|---------------------------------------------------|
| In1, In2   | Raw HW capture from pi (guitar pre-pedalboard)   |
| ModOut1/2  | Post-mod-host wet (the pedalboard tone)          |
| **Out1/2** | Stereo monitor return back to the pi             |


### PiStomp Companion

`PiStompCompanion.app` (installed to `/Applications`) shows the stack at a
glance from the menu bar: pi-Stomp reachability (icon dims when unplugged),
a badge for audio state (amber = reachable but not in the JACK graph,
green = linked/streaming, red = shm protocol mismatch), start/stop/restart
shortcuts, and one-click SSH / MOD-UI. **Network Diagnostics…** collects a
battery of unprivileged probes into `~/Library/Logs/JackBridge/` and opens
it in Console.app — run it when the pi won't connect and include the log in
any bug report. The app only *reads* the shared-memory region; it never
contends with the daemon.

---

## When it doesn't work (Troubleshooting)

### 1. I lost Internet on my Mac!
macOS often prioritizes the Ethernet cable over Wi-Fi. Since the Pi has no internet gateway, your Mac gets "stuck" trying to use it.
*   **Fix:** System Settings > Network > ... (three dots) > **Set Service Order...** > Drag **Wi-Fi** above your Ethernet (sometimes "10/100/1000") device.

### 2. Services aren't starting (or I see the wrong ports)
*   **Conflicts:** If you have **MOD Desktop**, **Jamulus**, or **SONABUS** running, they might have started their own "default" JACK server. JackBridge will accidentally connect to theirs instead of its own managed one.
    *   **Fix:** Quit those apps, then run `jackbridge-ctl restart`.
*   **Check the logs:** `jackbridge-ctl logs`

### 3. Pi ports don't appear in `jack_lsp`
It's almost always the multicast route landing on the wrong interface (Wi-Fi instead of Ethernet).
```sh
# Force the route watcher to re-pin the interface
sudo launchctl kickstart -k system/com.jackbridge.route
```

### 4. Audio is silent but ports are visible
If `jack_lsp` shows `pistomp` ports but you hear nothing:
*   **Check Connections:** Run `jack_lsp -c`. Ensure the `pistomp` ports are actually connected to `JackBridge` ports. If not, check `AutoConnect` in your `config.plist`.
*   **Wait for Sync:** netJACK2's resampler can take 5–10 seconds to stabilize on a fresh connection.
*   **Pi-Side Check:** SSH into the Pi (`ssh pistomp@pistomp.local`) and run `jack_lsp`. If the Pi doesn't see its own `system` hardware ports, it has nothing to send to the Mac.
*   **Restart Pi-Stomp:** Sometimes the internal audio engine (`mod-host`) needs a kick. Toggle the "Ethernet Audio" setting off and on again.

### 5. Audio is distorted or has "clicks"
*   **XRuns:** Check the logs (`jackbridge-ctl logs`). If you see `JackEngine::XRun`, your latency settings are too aggressive.
*   **Fix:** Increase `PeriodFrames` in `config.plist` (try 128). Avoid raising `JitterFrames` — it defaults to 0 and the multicast-pin path means we no longer need a HAL-side safety lead.

### 6. jackd won't start (or doesn't appear in `jackbridge-ctl status`) on Wi-Fi
This is intentional, not a bug. The route daemon only enables jackd when a wired/direct-cable interface is up — netJACK2 at 48 kHz × 4 channels will saturate a typical 2.4 GHz link and produce constant xruns. There is no override. Plug an Ethernet cable in (or attach a USB Ethernet adapter) and jackd starts automatically within ~2 seconds. See `docs/architecture.md` for the clock-domain rationale and why we don't add SRC.

---

## Installation

Get the latest release from the [Releases page](https://github.com/treefallsound/pistomp-companion/releases).
Each release attaches two `.pkg` files — install both, in order:

1. **`jack2-<version>.pkg`** — the JACK2 fork we depend on. Stock
   `jackaudio/jack2` 1.9.22 is missing the multicast-interface pin; without
   this fork, netJACK2's discovery times out on hosts with both wifi and a
   direct-cable NIC. The package installs to `/usr/local`.
2. **`PiStompCompanion-<version>.pkg`** — the Companion app, HAL driver,
   `JackBridged` daemon, LaunchAgents, route watcher, and `jackd-launch`
   wrapper. Double-click and run. A signed release should pass Gatekeeper
   without the unsigned-package workaround used by local builds.

Re-running the same package is safe — the postinstall preserves a hand-edited
`config.plist` and only re-bootstraps the LaunchAgents.

### Building Tools (Optional)
If you are working from source, compile the helper utilities:
```sh
gcc -O2 jackbridge/tools/rmshm.c -o jackbridge/tools/rmshm
gcc -O2 jackbridge/tools/chkshm.c -o jackbridge/tools/chkshm
```

For the pi side: install pistomp-arch with JackBridge enabled, plug Ethernet from Mac to pi, toggle "Ethernet Audio Interface" on the LCD.

## Configuration

`/Library/Application Support/JackBridge/config.plist` — saving it kicks the LaunchAgents (WatchPaths).

- `ClockDeviceUID` — CoreAudio UID for jackd's backend device. Empty = auto-detect built-in output.
- `PeriodFrames` — The dominant latency knob; 64 or 128 is recommended.
- `NetworkInterface` — Name of the NIC to use. Empty = auto-detect (prefers 169.254.x).

## Architecture & Building

JackBridge is the engine inside PiStomp Companion. It is a fork of
[`madhatter68/JackRouter`](https://github.com/madhatter68/JackRouter),
modernized for Apple Silicon and customized for pi-Stomp.

Two processes, one POSIX shm region (`/JackBridge`), atomic sync. The
**driver** (HAL plugin) memcpys between the DAW and shm. The **daemon** (JACK
client) memcpys between JACK and shm. No SRC inside JackBridge; clock-domain
crossing is handled by netJACK2.

* [docs/architecture.md](docs/architecture.md) — Detailed design
* [docs/macos-setup.md](docs/macos-setup.md) — Edge cases
* [jackbridge/tools/jackbridge-ctl](jackbridge/tools/jackbridge-ctl) — Status/Stop/Start script
* [jackbridge/installer/build-pkg.sh](jackbridge/installer/build-pkg.sh) — Build the package

## License

See `LICENSE`. The JackBridge engine inherits from the upstream
`madhatter68/JackRouter` project.
