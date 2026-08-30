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

### The shm control region is hand-laid-out, and asserts say so
Field addresses in `jackbridge/shared/JackBridge.h` are literal offsets, not a
struct, so nothing but the `static_assert` block near `STRBUF_UP` stops two
fields from claiming the same bytes. That is not hypothetical: before protocol
7, `JB_OFF_DEVICE_NAME` started at `0x180`, the same address as
`JB_OFF_READ_FRAME_NUMBER(0)`. The daemon wrote 128 bytes of device name over
the per-stream frame counters at attach, and the HAL's IO thread then wrote the
counters back over the name. It only ever worked because the HAL reads the name
exactly once, in `_HW_Open`, before IO starts. The device name now lives at
`0x200`, past every atomic, and the asserts fail the build on the next overlap.
Add a new field by extending that assert block, not by picking a free-looking
address.

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

### A client RT thread must leave the CoreAudio workgroup before it ends
`JackClient::SetupRealTime` (`../jack2/common/JackClient.cpp`) joins the
backend device's `os_workgroup` on the client's realtime thread. Membership
does **not** drop by itself at thread exit: libdispatch raises
`EXC_BREAKPOINT` in `_os_workgroup_tsd_cleanup` during that thread's
`pthread_exit`, which kills jackd. `JackEngine::ClientDeactivate` cancels the
thread, so no ordinary return path runs and a plain call at the end of the run
loop never executes.

`JackPosixThread::ThreadHandler` therefore runs a thread-local exit hook from a
`pthread_cleanup_push` handler; cancellation handlers run before the
thread-specific-data destructors, which is the ordering that matters.
`SetupRealTime` registers the hook only when the join succeeded. The hook is
thread-local and reached through `JackSetThreadExitHook` rather than a direct
call, because `JackPosixThread.cpp` is compiled into libraries that do not
contain `JackWorkgroup.mm` — `netlib` fails to link a direct reference, and
`weak_import` does not help for a symbol absent at static link time.

Symptom before the fix: jackd died on **every** cable pull, with a report in
`~/Library/Logs/DiagnosticReports/jackd-*.ips` naming
`_os_workgroup_tsd_cleanup`. The whole restart cascade below it — daemon
restart storm, agent restart, gate, coordinator, netmanager reload — was
fallout from that crash.

### `SIGPIPE` used to stop the server
`jackctl_setup_signals` puts `SIGPIPE` in the sigwait set
(`../jack2/common/JackControlAPI.cpp`), and `jackctl_wait_signals` had no case
for it, so it fell to `default: waiting = false` and began shutdown. A write to
a peer that went away is routine for a server doing network I/O, and the code
already handles the `EPIPE` correctly (`connection lost … 'pistomp' exiting`).
The signal killed it anyway, and the shutdown then hung in `ClientDeactivate`
for the client that had just lost its peer. `SIGPIPE` now logs and keeps
waiting. It stays blocked in every thread, so writes still return `EPIPE`.

### jackd's own death message can be missing from the log
`jackd-launch` pipes jackd's stderr through a dedupe filter. A crash produces
no `Jack main caught signal` line at all, because the only path that prints one
is `jackctl_wait_signals`. Do not read its absence as "jackd exited cleanly",
and do not trust an exit status from `wait "$JACKD_PID"`: jackd is a pipeline
member, the shell does not own it as a job, and `wait` returns 0 whatever
happened. The unfiltered stream is teed to `/tmp/jackbridge-jackd.raw.log`
(rotated to `.prev` when the next jackd starts); the crash reports are the
other half.

## Replug recovery on the Mac

### A carrier drop is not an interface move
`jackd-launch`'s monitor loop restarts the agent when the wired interface
changes, because netmanager cannot rebind its multicast interface live. An
absent interface is **not** that case: pull the cable and put it back and the
name, the ifindex and the pin are all identical. Restarting anyway destroyed
jackd and the master the pi was still announcing to. The loop now waits out an
absent interface and compares the IPv6 scopeid (the ifindex) to catch a device
that was re-created, such as a USB adapter re-enumerated.

### The startup gate runs once, and cannot use `pistomp.local`
The gate sat above the `--launch-jack` dispatch, so it ran in the coordinator's
parent *and* again in its child — two full `LINK_WAIT_LIMIT` waits, 120 s of a
140 s recovery. It is now inside the branch that execs the coordinator.

Its probe pinged `pistomp.local`, which resolves to the pi's **Wi-Fi** address
only — mDNS advertises no address for the pi's `eth0`. `ping -b en7` to that
address fails with the same `No route to host` the netJACK2 master gets, so the
probe could never test the wired path it claimed to. The gate now waits for an
ARP-resolvable link-local peer on the wired interface, and keeps the ping as a
fallback for a deployment where the pi has a routed address.

### The ARP pinner must compare against the ARP table, not its own memory
`jackbridge-route-watcher` pins the pi's MAC as an interface-scoped static ARP
entry when `tcpdump` sees a discovery packet, and flushes every `169.254` entry
— including that pin — at the top of each generation. The listener used to
remember the last address it pinned and skip every later announce, so once a
newer generation flushed the entry, nothing rewrote it. Measured: the pi
announcing once a second into a Mac with no ARP entry for it for 64 s. The
listener now compares the packet against the live ARP table, and each
generation reaps the previous one's orphaned subshell and `tcpdump`.

### The floor is RFC 3927, not JackBridge
After the above, a replug costs 4–9 s of IPv4 link-local address probing (three
ARP probes 1–2 s apart plus two announcements) before en7 has a usable address,
plus up to 1 s of the pi's announce interval. During the probe window the
master's socket fails with `EADDRNOTAVAIL` and logs `Can't init new NetMaster`
once per announce; it is retrying, not stuck. Nothing in the stack is idle
there. A static address alias on the wired interface would remove it, at the
cost of hardcoding one machine's cable.

## netJACK2 slave reconnection — stale master entries

When the netJACK2 slave restarts, the Mac netmanager may retain a stale
master entry briefly. A short delay between unloading and reloading the
pi-side client gives the Mac time to clean it up.

**Mitigation in `jackbridge/pi/bin/jackbridge-pi-up:57-64`:** a delay between
`jack_unload` and `jack_load` handles this jack2/netJACK2 integration edge case.


## netJACK2 supersede livelock — the replug wedge

When the Mac netmanager kills a *live* netmaster on an incoming
SLAVE_AVAILABLE announce, the pi's netadapter socket stays `connect()`ed to
the dead master's port. Linux connected-UDP then refuses every replacement
master's SETUP with ICMP port-unreachable (the Mac sees `Recv fd = N err =
Connection refused`), until the pi's own recv timeout restarts it — after
which the next master succeeds, and the next announce kills it again.

This is a **wedge**, not a crash: every process stays alive, the pi keeps
announcing (~1/s), the Mac keeps building masters, and the graph never
stabilizes. Measured live: 478 master recreations in one session
(`New NetMaster started` → `superseding the live one` → refused SETUPs →
repeat), ~9,000 xruns/min on the pi from starved ring buffers, zero audio.
Each side's recovery action re-arms the other side's failure, so waiting
never converges.

Visible signature in the unfiltered log
(`/tmp/com.treefallsound.companion.jackd.out.log`): master `ID : N`
climbing every cycle, `NetMaster 'pistomp' already present — superseding the
live one` between each `New NetMaster started`. A `tcpdump -i en7 -n 'udp
port 19000 or icmp'` shows the pi answer exactly one SETUP per cycle then
ICMP-refuse the rest.

**Fix (2026-08-28):** `InitMaster` in `../jack2/common/JackNetManager.cpp`
ignores every announce for a slave that already has a master in the list.
`ReapDeadMasters()` is the only death path.

`IsSynched()` (`JackNetInterface.h`, `std::atomic<bool> fSynched`) was added
for a narrower rule — supersede a master that never synched, ignore one that
did — but nothing calls it. Do not describe that rule as shipped.
Correction 2b's socket-close-on-failed-Init in the same file is related but
insufficient alone — it tidies each iteration without breaking the cycle.

## Proxy-ARP poisoning on link-local unicast

The Mac's kernel can resolve a pi link-local address through the wrong
interface when Wi-Fi and the direct cable are both active. The route watcher
pins the multicast and host routes to the wired interface. macOS `ping -I`
does not provide that binding; use the route watcher instead.

## Companion app

### A POSIX shm mapping outlives the name it came from
On XNU, `shm_unlink` + recreate leaves the old mapping fully readable and
frozen at its last values — no fault, no error, just plausible-looking stale
numbers forever. That is what a package upgrade, a `jb-rmshm`, or a protocol
bump does to a Companion that mapped once at launch. There is no cheap
identity check either: `fstat` on a POSIX shm object reports `st_dev == 0`
and `st_ino == 0` (measured, not assumed), leaving `st_size` as the only
meaningful field, and that doesn't change across a recreate. So
`ShmReader.attach()` re-resolves and remaps the name on **every** 5 Hz poll,
and `StatusMonitor` additionally treats a *decrease* in `daemonAlive` or
`halInputReadHead` as a discontinuity — both counters are monotonic while
one region lives, so a drop means the region or the daemon behind it was
replaced and the previous heartbeat baseline is meaningless.

### "Streaming" requires pi-side evidence, never `halInputReadHead` alone
`halInputReadHead` is written by the HAL IO proc whenever *any* DAW pulls the
device (`SA_Device.cpp`), so on its own it says nothing about the pi. Before
protocol 8 that was the sole basis for the solid-green "Streaming" state, which
meant a dead `pistomp` client with ports still registered — cable out, pedal
stopped, netmanager stalling ~2 s per 2.67 ms cycle — showed solid green with
audible silence. `StatusMonitor.recompute()` now claims `.streaming` only when
all three protocol-8 signals agree: `slavePortsConnected == 6` (drops to 0 the
instant jackd reaps a departed pi), `driverFault` bit 0 clear (the driver is
not feeding the DAW `bzero`), and the `daemonXRuns` rate is sane (not a steady
netmanager stall). Otherwise it reports `.noAudioFromPi`, whose detail line
names the fix. Each health state also carries `healthSince` so the UI can tell
a few seconds of startup from twenty minutes of silent death.

### `StatusMonitor.State` is owned by one queue and published by copy
The shm poll, the attach retry, the `jack_lsp` poll, and the reachability
probe all funnel through `StatusMonitor`'s serial `stateQueue`; the UI never
reads the monitor. `onUpdate` hands the main queue an immutable copy of the
struct and `AppDelegate` renders only from that. Anything that reaches into
the monitor from the main thread reintroduces the race this replaced.

### Every Companion subprocess is bounded
`ProcessRunner.run` is the only way the app spawns anything. It drains both
pipes through `readabilityHandler` (a blocking `readDataToEndOfFile` before
`waitUntilExit` deadlocks on any child that fills the 64 KiB pipe buffer),
escalates SIGTERM → SIGKILL at the budget, and bounds the wait for
end-of-output too — a backgrounded grandchild inherits the pipe and holds it
open past the child's death. Worst case is `timeout + 3*grace`.

### One JACK prefix, stamped at build time
`build-pkg.sh` writes its `$JACK_PREFIX` into `config.plist` as `JackPrefix`,
and both runtime readers — `jackbridge/installer/jack-prefix.sh` (sourced by
`jackd-launch`) and `app/PiStompCompanion/JackTools.swift` — resolve it the
same way: `$JACKBRIDGE_JACK_PREFIX`, then `JackPrefix`, then a probe of
`/usr/local` and `/opt/homebrew`, then `/usr/local`. Nothing hardcodes
`/usr/local/bin/jack_lsp` any more. The daemon links `libjack` from the
build-time prefix, so a CLI tool from a different prefix is version skew
against the running server.

## Naming

The repository is **PiStomp Companion**. JackBridge is the engine and the
installed CoreAudio/JACK service name. The historical upstream project is
`madhatter68/JackRouter`.

The `SA_` prefix in `SA_Device.cpp` / `SA_PlugIn.cpp` is from Apple's "SimpleAudio" sample that this forked from.
