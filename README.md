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
- Two macOS packages: `jack2-<version>.pkg` and
  `PiStompCompanion-<version>.pkg`. Get them from a maintainer, or build them
  (see **Development**).
- Two pi packages: the `jack2` and `jackbridge` debs. A pi-Stomp image of
  3.3.0 or newer already has them.
- For development only: Xcode and [`just`](https://just.systems).

---

## Set up

Do these steps one time. Steps 1 to 3 are on the pi. Steps 4 to 8 are on the
Mac.

1. **Install the two debs on the pi. Install `jack2` first.** A pi-Stomp image
   of 3.3.0 or newer contains both debs.
2. **Do not start the jackbridge service on the pi.** The service is not
   enabled at boot. The Mac starts it and stops it.
3. **Open the network menu on the pi screen.** Select **Wired Connection**.
   Then enable audio streaming.
4. **Connect the Ethernet cable directly from the Mac to the pi.** Use a USB
   Ethernet adapter if the Mac has no Ethernet port.
5. **Move Wi-Fi above Ethernet in the network service order.** Go to System
   Settings → Network → "…" → **Set Service Order**. Drag **Wi-Fi** above the
   Ethernet device. The pi supplies no gateway. If the Ethernet device stays
   first, the Mac loses the Internet connection.
6. **Install the two macOS packages. Install `jack2` first.** The packages
   have no signature, and Gatekeeper refuses a double-click. Install them in
   Terminal:

   ```sh
   sudo installer -pkg ~/Downloads/jack2-<version>.pkg -target /
   sudo installer -pkg ~/Downloads/PiStompCompanion-<version>.pkg -target /
   ```

   The `installer` command does not do the Gatekeeper check. To install with
   a double-click, first remove the quarantine attribute with
   `xattr -c ~/Downloads/*.pkg`.
7. **Open PiStomp Companion.** The app starts the audio stack. The app has no
   Dock icon and no window. Its icon is in the menu bar.
8. **Send an ssh key to the pi.** At the first start, the app opens a Terminal
   window and runs `ssh-copy-id`. Type the pi password one time. The app then
   controls the pi with the key.

If the pi hostname is not `pistomp.local`, open **Settings…** in the menu.
Type the correct hostname. Two pi-Stomps on one network cannot use the same
hostname.

Open the DAW. Select **pi-Stomp (<host>)** as the audio device.

---

## Daily use

1. Connect the Ethernet cable directly from the Mac to the pi.
2. On the pi, open the network menu. Select **Wired Connection**, then enable
   audio streaming.
3. Open the app.
4. In your DAW, select **pi-Stomp (<host>)** as the audio device.

The app has no Dock icon and no window. Find the pi-Stomp icon in the menu
bar. The icon shows the connection state. The menu has start, stop, restart,
SSH, Deploy, MOD-UI, and diagnostics.

The app owns the service lifecycle. When you open the app, it starts the audio
stack. When you quit the app, it stops the stack. The pi holds no settings of
its own: the Mac sends the tuning values to the pi before each start.

### Audio channels in your DAW

| DAW channel | Source |
|-------------|--------|
| In1, In2    | Hardware capture from the pi, before the pedalboard |
| ModOut1/2   | The signal after the pedalboard |
| Out1/2      | The stereo monitor return to the pi |

---

## Update

To install a new version, use the commands in step 6 again. To repair an
installation, install the same package again. An installation keeps your
`config.plist`. Read the release notes for new configuration keys and new
default values.

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
Usually the netJACK2 loop budget is too small for the cable's worst-case round
trip. Both values live in the menu bar under **Settings… → Pi tuning**, and
the Companion writes them to the pi for you:

    Net latency (-l)    cycles of cushion
    Net ring (-g)       slip-ring frames

Raise them **together** — half the ring must stay above latency × period, or
the pi glitches instead of the Mac; Settings refuses a pair that cannot hold.
Click Apply, and give the link a few seconds. Every cycle of cushion costs
monitoring latency, so use the smallest pair that stays clean.

**No audio over Wi-Fi.**
This is by design, not a bug. Audio needs the Ethernet cable.
Fix: connect the cable, or attach a USB Ethernet adapter.

Full diagnostics and rare cases: `docs/macos-setup.md`.

### Reporting a problem

The menu bar has **Network Diagnostics…**. It collects the state of the link
and the audio engine into a file in `~/Library/Logs/JackBridge/` and shows you
where it went.

Run it **while the problem is happening**, not after you restart. The engine
records what its buffers are doing at the time; a restart clears it.

Attach that file to the bug report, and say what you heard and when. If a
recorded take is affected, name the take and the approximate time — the log
carries timestamps, so the two can be lined up.

Maintainers: the terminal side of this is `CLAUDE.md` §5.

---

## Development

### One-time machine setup

Building the engine needs jack2 development files on the build machine, and
the daemon at run time loads `/usr/local/lib/libjack.dylib`. Both come from
the [`TreefallSound/jack2`](https://github.com/TreefallSound/jack2) fork —
**stock jack2 is not sufficient.** The fork carries the netJACK2 slave
reaping, KillMaster UAF fix, and the multicast interface pin; without them
the engine permanently freezes its audio thread whenever the pi-Stomp side
restarts or disconnects (see `jackbridge/…` and `docs/idiosyncrasies.md` for
the exact failure mode).

If you have a development clone of the fork next to this repo:

```sh
just jack-rebuild           # rebuilds/installs JACK2 and restarts both ends
```

This builds the current fork, installs directly into `/usr/local`, and then
restarts the Mac and Pi bridge. It warns if the fork's working tree is dirty;
uncommitted changes are included in the build. For a fresh machine, install
the fork's `.pkg` from a release first; the release install path is the
supported one, this recipe is for iteration.

The command runner is [`just`](https://just.systems). Install it with
`brew install just`. Run `just --list` to see every command.

Recipes come in four layers, and each does only its own job:

| Layer | What it does | Examples |
|---|---|---|
| build | Compiles into the build tree. Nothing else. | `driver`, `daemon`, `engine`, `app` |
| install | Copies into system paths. Needs sudo. | `install-engine`, `install-scripts`, `install` |
| control | Acts on the running stack. | `restart`, `unlink-shm`, `status`, `logs` |
| loop | What you actually type. Composes the rest. | `reload`, `reload-all`, `reload-scripts`, `rmshm` |

One rule keeps them composable: **install never restarts, and control never
installs.** So you can chain any number of installs and then restart exactly
once. Two recipes that each restart must not be chained.

Day to day:

```sh
just                 # build, install, drop the shm, bounce the engine
just reload-all      # the same, plus the menu-bar app
just reload-scripts  # shell helpers only, no Xcode build
just status          # service health
just logs            # follow the engine logs
just device-name     # show the audio device entry
```

For a release package: `just pkg-install` (requires sudo). That is the
authoritative install path — the loops above are for iteration, and they
deliberately skip the files the package generates or templates.

### Build the packages

Build `jack2` first. `build-pkg.sh` compiles against the fork's headers at
`JACK_PREFIX`, and it stops with an error if the headers are not there.

```sh
cd ../jack2
./build-macos-pkg.sh 1.9.22-tfs.N
sudo installer -pkg build/jack2-*.pkg -target /
cd ../pistomp-companion
just pkg
```

`just pkg` writes
`jackbridge/installer/build/PiStompCompanion-<version>.pkg`. Send that file
and `../jack2/build/jack2-*.pkg` to the tester. The tester does not need this
repository, Xcode, or `just`.

### Signatures

`build-pkg.sh` reads `SIGN_APP_IDENTITY`, `SIGN_INSTALLER_IDENTITY`, and
`NOTARY_PROFILE`. If these variables are empty, the build makes an ad-hoc
signature on each binary and an unsigned `.pkg`.

An ad-hoc signature is sufficient to run the code. The driver, the daemon,
and the app start on a different Apple Silicon Mac.

An unsigned `.pkg` is not sufficient for Finder. Gatekeeper refuses a
double-click on a package that a browser downloaded. The `installer` command
does not do this check, thus **Set up** step 6 uses `sudo installer`. The
`installer` command also writes the files without the quarantine attribute,
and the app starts with no more prompts.

To make a signed and notarized package, set the three variables before the
build. See `docs/releases.md`.

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
- [docs/DEBUGGING.md](docs/DEBUGGING.md) — how to read `just shm` and the
  cadence counters, with a symptom index
- [docs/macos-setup.md](docs/macos-setup.md) — edge cases
- [jackbridge/tools/jackbridge-ctl](jackbridge/tools/jackbridge-ctl) —
  status/stop/start script
- [jackbridge/installer/build-pkg.sh](jackbridge/installer/build-pkg.sh) —
  build the packages

The user-facing install, diagnostics, and troubleshooting reference is in
`docs/macos-setup.md`. The architecture is in `docs/architecture.md`.

---

## License

See `LICENSE`. The engine is a fork of
[`madhatter68/JackRouter`](https://github.com/madhatter68/JackRouter).
