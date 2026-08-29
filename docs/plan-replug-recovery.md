# Plan — Recovery after an Ethernet replug

Written in ASD-STE100 Simplified Technical English.

The subject is this fault: the user disconnects the Ethernet cable and connects
it again, and the audio does not come back without help.

## Status — 2026-08-28

Fault 1 is corrected and is tested on hardware. Fault 2 is corrected in code
and is not yet tested on hardware. Task C is done.

| Item | State |
|------|-------|
| Change A — the device stays alive | done, correct |
| Fault 1 — the daemon writes a driver field | corrected, tested |
| Fault 2 — the netJACK2 master does not start again | corrected in code, hardware test open |
| Task C — reduce the menu to one recovery verb | done |

## Fault 1 — the daemon wrote a field that the driver owns — CORRECTED

### The cause

`shmDriverStatus` (offset `0x128`) has one correct writer: the driver.
`_HW_Open` writes `ACTIVE`. `_HW_StartIO` writes `STARTED`. `_HW_StopIO`
writes `ACTIVE`.

The daemon also wrote this field. `on_shutdown` wrote `INIT`.

The sequence was:

1. The cable fault stopped jackd.
2. `on_shutdown` wrote `shmDriverStatus = INIT`. The daemon stopped.
3. The LaunchAgent started a new daemon.
4. The new daemon read `INIT` at `JackBridge.cpp:351`. The process callback
   made the output buffers zero and returned. The callback did not touch the
   ring buffers.
5. Only `_HW_StartIO` writes `STARTED`. Change A correctly prevents the
   teardown that called `_HW_StartIO`. Thus the value stayed at `INIT` for an
   unlimited time.
6. The condition continued until the user selected the device again in the
   DAW. That selection called `_HW_StartIO`.

One field explains all the symptoms: no audio (step 4); a green outline and
not a filled dot (the app shows the same field); **Repair Audio Link** does
nothing (it acts on the driver, but the daemon was the stopped part); the DAW
gives the cure (`_HW_StartIO` writes `STARTED`).

### The corrections

| # | Change | Location |
|---|--------|----------|
| 1 | The daemon no longer writes `shmDriverStatus`. It keeps its own `shmDaemonAlive` write. | `jackbridge/daemon/JackBridge.cpp:449` |
| 2 | The driver compares `shmDriverStatus` with `mDriverStatus` each IO cycle and writes its own value on a difference. | `jackbridge/driver/JackBridge/Plug-In/SA_Device.cpp:1526` |
| 3 | `mDriverStatus` is `std::atomic<UInt32>`. The IO thread reads it and the control threads write it. The previous plain `UInt32` was a data race. | `jackbridge/driver/JackBridge/Plug-In/SA_Device.h:189` |
| 4 | The resync value comes from `mach_absolute_time()`. | `app/PiStompCompanion/AppDelegate.swift:26` |
| 5 | A field-ownership table records one writer for each field. | `jackbridge/shared/JackBridge.h:62` |

Change 2 makes the fault impossible and not only unlikely. An old daemon build
has the same protocol version and can still write `INIT`. The driver corrects
the value in the next IO cycle.

Change 4 is necessary because the driver keeps the last resync value for as
long as the plug-in stays loaded. That time is longer than one run of the app.
A counter that starts at 0 at each start of the app can thus send a value that
the driver already has. **Repair Audio Link** then does nothing.

### The test result

The operator disconnected the cable and connected it again. The operator did
not touch the DAW.

| Item | Result |
|------|--------|
| `driverStatus` during the fault | stayed at 2 (`STARTED`) |
| `StartIO` calls in the test period | none |
| Driver host process ID | 51874, no change |
| Daemon process ID | 52048 → 53442 → 54701 |

No `StartIO` occurred. Thus the recovery did not use the DAW. Fault 1 is
corrected.

## Fault 2 — the netJACK2 master does not start again — OPEN

**Do this first. This is now the cause of no audio after a replug.**

### What occurs

`jackd-launch` contains a self-healing loop. When the wired interface changes,
the loop stops jackd. The LaunchAgent then starts jackd again. In the test this
loop operated at 19:44:51, during the replug, while the interface was not yet
stable.

netmanager then failed. Each attempt to make a master gave this result in the
Mac jackd log:

    Recv fd = 6 err = Connection refused
    Problem with network
    JackNetMasterInterface::Init() error...
    Can't init new NetMaster...

The loop continued for 2 minutes. The loop does not stop by itself.

At the same time the pi log showed a full handshake and then a failure:

    Waiting for a master...
    Initializing connection with Cams-MBP...
    **************** Network parameters ****************
    ... (the parameters are correct)
    Recv fd = 60 err = Resource temporarily unavailable
    Recv connection lost error
    NetAdapter is restarted

Thus the multicast announce operates and the parameter exchange operates. The
unicast data socket does not operate. The master receives ECONNREFUSED. The
slave receives EAGAIN.

The network was correct during the fault: the Mac had 169.254.125.10 on `en7`,
the pi had 169.254.125.193 on `eth0`, the multicast group routed out of `en7`
at the two ends, and `ping` operated.

### What is not the fault

- A restart of the pi service does not correct the fault. The slave makes a new
  connection each time (the log showed a new `ID`), and the master refuses each
  time.
- The message `auto-wire: jack_connect ... failed rc=-1` in the daemon log is
  correct and is not a fault. jackd also records `Cannot connect ports owned by
  inactive clients: "pistomp" is not active`. netmanager made the client but
  did not make it active. Do not remove these messages. They are the honest
  report of fault 2.

A restart of jackd corrects the fault immediately.

### Also seen

    jack_port_set_latency_range called with an incorrect port 573325344

This is an invalid pointer. Examine this in the fork. It can show a
use-after-free in the same path.

### Correction 2a — make the gate wait for a usable link (this repository)

File: `jackbridge/installer/jackd-launch`.

The function `wired_iface()` at line 27 gives an interface name when two
conditions are true: the sentinel file `/var/run/jackbridge-ethernet.up` is
present, and `/var/run/jackbridge-route.iface` is not empty. The two conditions
become true immediately when the link comes up. The link cannot carry data at
that moment.

The gate at line 74 uses only this function. The double-check in the
self-healing loop at line 225 makes the *decision to restart* stable. It does
not make the *interface* stable before jackd starts.

Do these steps:

1. In the gate, after `wired_iface()` gives a name, test that the pi answers.
2. Continue to wait while the test fails.
3. Do not wait for an unlimited time. Give a limit, and record a message when
   the limit is reached. A jackd that records nothing is worse than a jackd
   that starts and fails loudly.
4. Keep the test cheap. The gate operates at each start of the agent.

Note: `jackbridge-coordinator` probes the pi for its timing values. That probe
operates *after* the gate. Thus the gate cannot use it. Also, a probe that
fails there gives stale cached values and no message.

### Correction 2b — make netmanager recover (`../jack2`)

netmanager must recover from a failed `JackNetMasterInterface::Init()`. It must
not continue with a socket that does not operate. It must also remove the
client that it made but did not make active.

While in this code, chase the `jack_port_set_latency_range ... incorrect port`
message from "Also seen". It is in the same failed-Init path and can be a
use-after-free.

Correction 2a hides fault 2 for the common case. Correction 2b removes it. Do
2a first, because 2a is in this repository and a test is possible immediately.

## Task C — Reduce the menu to one recovery verb — OPEN

### What the menu has now

The menu has five verbs that act on the stack:

| Menu item | What `jackbridge-ctl` does |
|-----------|----------------------------|
| Start JackBridge | enable and bootstrap the two agents; start the pi service |
| Stop JackBridge | stop the pi service; bootout and disable the two agents |
| Restart JackBridge | stop, then start |
| Repair Audio Link | write a nonce to `JB_OFF_RESYNC_REQUEST` |
| Full Repair (restarts audio) | `sudo killall coreaudiod`; `sudo killall` the plug-in host; enumerate; `kickstart -k` the two agents; start the pi service |

Five verbs give three behaviours. Two verbs are not necessary.

### Repair Audio Link does almost nothing

The driver answers the nonce with three actions: it makes
`gDevice_AnchorHostTime` zero, it makes `mDaemonLive` true, and it makes
`driverFault` zero.

The first action has no effect. `syncMode` is 1, thus the daemon owns the
timeline and the driver globals are not used.

The other two actions now occur automatically. The heartbeat-resume branch in
`GetZeroTimeStamp` does the same two actions when the daemon starts to send
the heartbeat again. This is the correction for fault 1.

Thus the menu item gives the user no action that does not occur without it.
The 2026-08-28 test agrees: the operator clicked the item and nothing changed.

### Full Repair is the same as Restart, because its unique part fails

`sudo` has no terminal when the app starts `jackbridge-ctl`. Thus the two
`killall` commands fail. `|| true` hides the failure.

What remains is `kickstart -k` of the two agents and a start of the pi service.
`Restart JackBridge` gives the same result. Thus the two items are the same
item with different words.

### But the privileged part has one use

A bounce of `coreaudiod` is the only method to correct this condition: the HAL
plug-in is not loaded, thus `/JackBridge` does not exist, thus there is no
device and the daemon repeats `attach_shm failed` every 2 s.

This condition is real. It occurred on 2026-08-28. Refer to "A trap in the test
loop" below.

The cause on that day was a maintainer action (`just reload`), and a user
cannot do that action. It is not known if a user can reach this condition by a
different route. A crash of `coreaudiod` or an update of the operating system
can possibly do it. **Do not make a change for this condition until a user
reports a device that is not present.** Do not invent the requirement.

### The changes to make

1. Remove **Repair Audio Link** from the menu, and remove `repairLight`.
   Keep `nextResyncNonce()` and keep the driver code that answers the nonce:
   both are correct, and a future automatic re-anchor can use them.
2. Remove **Full Repair (restarts audio)**.
3. Keep **Restart JackBridge**. It becomes the one recovery verb.
4. Give `jackbridge-ctl repair` to the maintainer only. Do not put it in the
   menu. Record in the help text that it needs a terminal for `sudo`.
5. Correct the user table in `CLAUDE.md` section 4b. It gives **Repair Audio
   Link** and then **Full Repair** as the two steps. After this change the
   table has one step: **Restart JackBridge**.

Result: three stack verbs (Start, Stop, Restart) and one recovery verb, which
is Restart. The user has one thing to try, and it is the thing that operates.

### If the privileged bounce becomes necessary

Do not use a password window. Use the method that the route watcher uses. The
route watcher (`com.treefallsound.companion.route`) is a LaunchDaemon, it
operates as root, and it starts when a monitored file changes (`WatchPaths`).
Let the app write a file, and let the LaunchDaemon stop `coreaudiod` and the
plug-in host process. Add the action to **Restart JackBridge**. Do not add a
sixth menu item.

## How to observe

A cold reader needs these. No `just` recipe gives them.

| Item | Command |
|------|---------|
| shm control fields | `just shm` |
| Engine log (driver and daemon) | `just logs` |
| Mac jackd stderr | `tail -40 /tmp/com.treefallsound.companion.jackd.err.log` |
| Mac jackd stdout | `/tmp/com.treefallsound.companion.jackd.out.log` |
| JACK graph with connections | `JACK_NO_START_SERVER=1 /usr/local/bin/jack_lsp -c` |
| The pi, over ssh | `ssh pistomp@pistomp.local` |
| The pi status | `/usr/local/libexec/jackbridge/jackbridge-pi-status` |
| The pi jackd log | `journalctl -u jack -n 40 -o cat` |
| CoreAudio plug-in load faults | `log show --last 10m --predicate 'process == "coreaudiod"'` |
| The plug-in host process | `pgrep -fl "Core Audio Driver"` |

The Mac jackd stderr passes through a filter in `jackd-launch`. The filter
prints each distinct message one time in each 10 s window and adds
`[+N similar suppressed]`. Thus a count in that file is not a true count.

To read the shm control fields, use `just shm`. It prints `driverStatus`,
`syncMode`, the heartbeat, `slavePortsConnected`, the fault bits and the
device name. It is read-only and is safe while audio operates. Use it when the
link is up, the ports are wired, and there is no audio: it shows which of the
two processes has the incorrect state.

`syncMode` in that output is important. The value is 1, and 1 means that the
daemon owns the timeline. In this mode `GetZeroTimeStamp` only relays the shm
values, and the driver globals `gDevice_AnchorHostTime` and
`gDevice_NumberTimeStamps` have no effect. An earlier version of this plan gave
an incorrect cause because it did not know this.

**Network Diagnostics…** in the menu gives the same values and more probes, in
`~/Library/Logs/JackBridge/`. Use it for a bug report. Use `just shm` to work.

## How to test

Make the machine ready:

1. Run `just reload-all`. **Run it in a terminal.** The recipe uses `sudo`, and
   `sudo` cannot read a password when an agent starts the recipe.
2. Make sure that the device is present: `just device-name`.
3. Make sure that the graph is complete: `jack_lsp -c` shows six connections
   between `pistomp` and `JackBridge #1`.

Do the test:

4. Play audio in the DAW through the pi-Stomp device.
5. Disconnect the Ethernet cable. Wait 10 s.
6. Make sure that the DAW keeps the device.
7. Connect the cable again. Wait 15 s. **Do not touch the DAW.**
8. Make sure that audio starts again.
9. Make sure that the menu shows a filled green dot.
10. Make sure that `just logs` shows no `StartIO` in the test period.
11. Make sure that the driver host process ID has no change.

Step 10 and step 11 are the necessary controls. `_HW_StartIO` corrects fault 1
by itself, and a restart of `coreaudiod` corrects almost everything. A test
that permits either one proves nothing. A change of the daemon process ID is
correct and is expected: the LaunchAgent starts a new daemon. Refer to
`docs/plan-b-daemon-survives-jackd.md`.

To test change 4 (the resync value): Task C removes the menu item that wrote
the nonce, and nothing writes `JB_OFF_RESYNC_REQUEST` at the moment. Keep the
machinery: `nextResyncNonce()` in the app and the driver answer in
`SA_Device.cpp`. When a future automatic re-anchor writes the nonce again,
check that `just logs` shows `resync request ... honoured`.

## A trap in the test loop

`just install-engine` previously used `cp` onto the installed bundle. `cp`
writes through the existing inode. When the plug-in host had that inode in
memory, the kernel found a code-signing time that did not agree and refused
each page:

    CODE SIGNING: rejecting invalid page ... (cs_mtime:… != mtime:…) tainted:1
    Error loading driver bundle JackBridgePlugIn.driver
      Couldn't communicate with a helper application.

The result is no plug-in, no `/JackBridge` region, no device, and a daemon that
repeats `attach_shm failed` every 2 s. This looks the same as a wrong
`unlink-shm` order and is not that.

The recipe now removes the bundle before it copies, and it installs the two
binaries with `mv` (`justfile:102`). If you see this failure again, examine the
install recipe first.

## Risks

**Two writers in a race.** Change 2 lets the driver write `shmDriverStatus` in
each IO cycle. A control thread writes the same field at the same time. The two
writers agree, because change 2 writes only the value of `mDriverStatus`, and
the control threads set `mDriverStatus` before they write the shm field. The
result is the same value in each order.

**A stale value for one cycle.** The driver corrects the field in the next IO
cycle and not immediately. The audio is silent for that time. One cycle is
2.7 ms at 48 kHz and 128 frames.

**A gate that waits too long.** Correction 2a adds a wait. A wait with no limit
gives a stack that starts nothing and records nothing. Give a limit.

## Related

- `docs/plan-b-daemon-survives-jackd.md` — the daemon stays alive when jackd
  stops. That plan removes the daemon restart that made fault 1 visible.
- `CLAUDE.md` section 4, rule 6 — one writer for each shm field.
- `CLAUDE.md` section 4b — recovery has two audiences.
- `CLAUDE.md` section 1 — the `install → unlink-shm → restart` order.
- Change A: `SA_Device.cpp`, `kAudioDevicePropertyDeviceIsAlive`.
