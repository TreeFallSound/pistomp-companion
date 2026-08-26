# Idiosyncrasies & gotchas

The things that will surprise you. Each item has a file:line citation where applicable.

## Repo / build

### Shared contract is centralized
`jackbridge/shared/JackBridge.h` is the IPC contract included by both the
daemon and HAL targets. The companion mirrors the read-only control offsets
through its C/Swift reader; update it whenever the shared layout changes.

### `jackbridge/tools/rmshm.c` removes stale regions
The utility unlinks `/JackBridge` and the legacy `/jackrouter` names. Use it
when a dirty daemon exit leaves the old POSIX shm region behind.

### Xcode projects own the Mac build
The daemon and HAL build through
`jackbridge/driver/JackBridgePlugIn.xcodeproj`; the Companion has its own
project under `app/`. `jackbridge/installer/build-pkg.sh` assembles the
signed/notarized product package.

### Apple Silicon package
The released package is arm64 because the JACK2 dependency used by the
release pipeline is single-architecture. Local Xcode builds may produce both
architectures, but that does not make the complete package Intel-compatible.

## IPC / synchronization

### Shared fields are atomic
The daemon and HAL use `std::atomic<uint64_t>` with explicit memory ordering,
including the protocol version, heartbeat, frame cursors, and HAL anchor.
The companion maps the region read-only and only samples the control area.

### Protocol mismatch is intentional
`JACKBRIDGE_PROTOCOL_VERSION` is published into fresh shm and both service
processes refuse to attach when the observed version differs. Remove the shm
region with `jackbridge/tools/rmshm` after installing incompatible binaries.

### JACK shutdown is surfaced
The daemon marks its heartbeat/status appropriately and exits on JACK
shutdown; launchd owns the restart policy. The HAL marks itself unavailable
when the heartbeat stalls.

## Channel count, sample rate, format

### "Streams" are stereo pairs, not channels
`jackbridge/shared/JackBridge.h` defines `NUM_INPUT_STREAMS=2`,
`NUM_OUTPUT_STREAMS=1`, `MAX_STREAMS=2`, and `MAX_CHANNELS=4`. The device is
4-in / 2-out, organized as two stereo input streams and one stereo output
stream.

### Hardcoded `* 2` and `8` bytes per frame
The daemon and HAL frame addressing assumes stereo float32. Audit every
`*2` and `8` literal before changing stream widths or sample formats.

### `STRBUFSZ=32KB` = 4096 stereo float frames
At 48k/256-buffer that's approximately 21 ms of headroom. Plenty under
Config B (same clock domain). If anyone bumps to 96k or changes buffer sizing,
recompute.

## Clock sync

### Clock sync assumes the selected topology
`jackbridge/driver/JackBridge/Plug-In/SA_Device.cpp` reports timing from the
HAL anchor published in shared memory. Mac jackd must use the CoreAudio
backend; netJACK2 performs the Pi↔Mac clock crossing at the network boundary.
There is no SRC inside JackBridge.

### Frames-per-buffer and JACK period must agree
The package defaults both sides to 64-frame periods. Changing one side alone
can make netJACK2's resampler unstable; change the Pi and Mac settings together.

## CoreAudio / HAL

### Realtime IO paths are constrained
The HAL IO operations and daemon JACK callback must remain allocation-free and
avoid syscalls, logging, and heavyweight locks. The current implementation
uses the Apple utility classes and memcpy-based ring transfers.

### Bundle path is fixed
`/Library/Audio/Plug-Ins/HAL/JackBridgePlugIn.driver`. After installing,
`coreaudiod` is restarted by the package postinstall. Release packages must
be signed and notarized for normal Gatekeeper installation.

## Aggregate device pitfall

If a user creates an aggregate device that **includes JackBridge** as a sub-device and points jackd at that aggregate, CoreAudio does not detect the cycle. You get silence, hard mute, or runaway depending on buffer ordering — never a clean error. Defensive check in the daemon (Phase 2): enumerate the aggregate jackd is bound to via `kAudioAggregateDevicePropertyActiveSubDeviceList` and refuse to start if our UID is in it.

## jackd on macOS

### Default realtime priority is 10
`jackd -R` on macOS starts at priority 10 unless `-P N` is given explicitly. The Pi-side default (running as a service) is 75. With the master at 10 and any browser / DAW / coreaudiod work going on, the netJACK2 master client misses deadlines constantly and slaves disconnect. `jackd-launch` must pass `-P 75` (or higher). See `spike-b-clock-stability.md`.

### `jackd -d coreaudio -d "<friendly name>"` is silently ignored
The user-visible device name (e.g. `"Steinberg UR22C"`, the same string Audio MIDI Setup shows) does not select the device. jackd falls back to "default input + default output" and, if those differ, auto-creates a cross-clock aggregate with a `clock drift compensation would be needed` warning — which is exactly the cross-clock topology Config B forbids. Use the internal CoreAudio name from `jackd -d coreaudio -l` instead (e.g. `AppleUSBAudioEngine:Yamaha Corporation:Steinberg UR22C:120000:1,2`). For the production aggregate, this is `~:<aggregate-uid>` as documented in `macos-setup.md`.

### `jackd -d coreaudio` does **not** take exclusive control of the device
Even while jackd is bound to a CoreAudio device, that device remains available as a system output — apps can play through it and the audio mixes with whatever's going through jackd. Useful (system audio keeps working during development) but a trap: **if the user sets the same device as both jackd's backend and the system output, they get a feedback loop / mix-of-everything** with no clear error. The aggregate-device strategy (built-in output as the aggregate's sub-device) avoids this for production, since the user is unlikely to pick the aggregate as their normal system output. Pass `-H`/`--hog` to force exclusive access if needed; we don't, to allow side-by-side dev workflows.

## netJACK2 slave reconnection — stale master entries

When the netJACK2 slave restarts, the Mac netmanager may retain a stale
master entry briefly. A short delay between unloading and reloading the
pi-side client gives the Mac time to clean it up.

**Mitigation in `jackbridge/pi/bin/jackbridge-pi-up:57-64`:** a delay between
`jack_unload` and `jack_load` handles this jack2/netJACK2 integration edge case.

## Proxy-ARP poisoning on link-local unicast

The Mac's kernel can resolve a pi link-local address through the wrong
interface when Wi-Fi and the direct cable are both active. The route watcher
pins the multicast and host routes to the wired interface. macOS `ping -I`
does not provide that binding; use the route watcher instead.

## Naming

The repository is **PiStomp Companion**. JackBridge is the engine and the
installed CoreAudio/JACK service name. The historical upstream project is
`madhatter68/JackRouter`.

The `SA_` prefix in `SA_Device.cpp` / `SA_PlugIn.cpp` is from Apple's "SimpleAudio" sample that this forked from.
