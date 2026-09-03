# Plan: Mac-authoritative tuning

Written 2026-08-30. This is the only tuning plan. It replaces three earlier
plans, which are deleted: the network delay plan, the ethernet jitter plan,
and the latency authority plan. Every measurement they hold that is still
true is repeated here.

## 1. Why this is one plan

The three earlier plans were views of one problem. Each of them stopped at a
step that another one owned. Each described the same fault: a parameter lives
on the pi, the Mac does not know it, and no measurement is repeatable.

This plan states the goal directly.

**The Mac configures the pi completely. The pi holds no setting of its own.**

Every parameter that changes timing is a key in `config.plist` on the Mac.
`jackbridge-ctl` writes those values to the pi before each service start. A
reimage of the pi then costs nothing, and a measurement is repeatable,
because the configuration is one file on one machine.

## 2. What is true now

Measured on 2026-08-30 against the working tree and the live pi.

### 2.1 The pi runs L=4 and G=1024

    pistomp@pistomp.local:/etc/default/jackbridge
    JACKBRIDGE_NET_LATENCY=4
    JACKBRIDGE_NET_RING=1024

An earlier plan recorded the live values as `-l 2` and `-g 512`, which were
the script defaults after a reimage. That is no longer correct: a person wrote
the file by hand afterwards. The conclusion does not change, because the file
is in no repository, so a reimage still removes it.

### 2.2 The Mac advertises a wrong latency

`JB_NET_LATENCY_CYCLES` is 2 in `jackbridge/shared/JackBridge.h:227`. The pi
runs 4. The constants and the machine disagree.

    one_way = JB_CODEC_GROUP_DELAY_FRAMES
            + (JB_ALSA_PERIODS_PI + L + 3) * period
            + G / 2
            + wire

    at period 64, 48 kHz, wire = 17:

    source tree        L=2  G=1024   ->   978 frames
    installed binary   L=2  G= 512   ->   722 frames
    the truth          L=4  G=1024   ->  1106 frames

The DAW (Digital Audio Workstation) is told 722 and the link costs 1106. The
error is **384 frames, which is 8.0 ms per leg**, and the monitoring trip
carries it twice.

The installed binary is worse than the source tree, because it is stale. It
was built when `JB_NETADAPTER_RING_FRAMES` was 512, and the header now holds
1024. The daemon confirms this at startup:

    latency model: period=64 f_s=48000 -> one-way=722 frames

**Rebuild the engine before any measurement.** The running daemon also emits a
health line whose format does not match `JackBridge.cpp:884` in the tree, so
the installed engine and this repository are not the same code. Nothing in
this plan can be trusted against a binary that the source does not describe.

### 2.3 None of the Mac-side ownership is built

`NetLatency` and `NetRing` appear in no file. The keys are absent from
`config.plist`, from `jackbridge-ctl`, from the daemon, and from the Settings
window. `JACKBRIDGE_NET_LATENCY` and `JACKBRIDGE_NET_RING` are read only by
`jackbridge/pi/bin/jackbridge-pi-up`.

### 2.4 The pi has one configuration channel already

The unit file reads `EnvironmentFile=-/etc/default/jackbridge`. The helper
scripts read `JACKBRIDGE_*` variables from that environment. The channel is
correct. Only two knobs use it. Every other tuning value in the helpers is a
constant in the script.

### 2.5 The packet path has the lowest priority on the pi

Live values, read from the running pi. `psr` is the CPU the thread was on at
that moment.

| Thread | Policy | Priority | psr | Role |
|--------|--------|----------|-----|------|
| jackd audio | SCHED_FIFO | 75 | 3 | ALSA cycle |
| jackd netadapter (x2) | SCHED_FIFO | 70 | — | send and receive to the Mac |
| mod-host | SCHED_FIFO | 65 | 1 | the plugin graph |
| `napi/eth0-5`, `napi/eth0-6` | SCHED_FIFO | 60 | 2, 1 | packet polling |
| `irq/104-eth%d` | SCHED_FIFO | 50 | 0 | the eth0 interrupt handler |

The kernel is a threaded-interrupt kernel, so every hardware interrupt runs in
a thread with a priority. The eth0 interrupt thread has priority 50. That is
the lowest realtime priority on the machine.

**The whole packet path ranks below the work that depends on it.** mod-host at
65 preempts the NAPI threads at 60, and it preempts the interrupt thread at
50. A packet can therefore wait for a plugin quantum before the kernel even
looks at it.

This is the inversion. Paragraph 3.4 acts on it, and only on it.

### 2.6 The power supply result is not valid

`vcgencmd get_throttled` returns `0x0`. The pi had been running for less than
one minute. That register clears at boot. The reading proves nothing. A valid
reading needs one hour of audio first.

### 2.7 No thread is pinned to a CPU

Every jackd realtime thread, both `napi/eth0-*` threads and the eth0 interrupt
thread report `cpus=0-3`. `/proc/irq/104/smp_affinity_list` is `0-3`.

The kernel therefore places the packet path wherever it likes, and it does not
keep the path together. In the reading above, the interrupt thread was on CPU0,
one NAPI thread was on CPU2, and the other NAPI thread was on CPU1 with
mod-host.

### 2.8 The workgroup question has a confound

`docs/JITTER.md` holds the measurement:

| Workgroup | DAW buffer | -l | Mac xruns / 5 s |
|-----------|-----------|----|-----------------|
| hal | 128 | 2 | 39.6 |
| none | 64 | 2 | 19.0 |
| backend | 64 | 2 | 12.5 |
| hal | 64 | 2 | 7.3 |

The buffer size moved the result by a factor of 5. The workgroup choice moved
it by a factor of 2. The buffer size is the larger effect. The HAL driver now
advertises `kAudioDevicePropertyBufferFrameSize` and its `32..1024` range,
with a 512-frame default independent of the JACK/netJACK period. A host may
choose another HAL N without changing the matched JACK P.

The table above is historical: it measured the old generic 128-frame default
and the experimental matched 64-frame setting. Re-run the workgroup comparison
after installing this driver, using the active HAL N as part of the test label.

### 2.9 The timeline can diverge, and nothing brings it back

Observed on 2026-08-30. The audio became noise while every status field looked
healthy: `driverStatus` was `STARTED`, `driverFault` was 0, the heartbeat
advanced, and `slavePortsConnected` was 6 of 6.

The daemon health lines hold the fault. `deficit` is in frames.

    old daemon
    13:14:01  xruns=0  deficit=484136464  snaps=52
    13:14:41  xruns=0  deficit=228045140  snaps=21
    13:15:21  xruns=0  deficit=188862908  snaps=15
    13:15:33  caught signal 15, shutting down

    new daemon
    13:15:40  JackBridge#0: re-anchored after HAL restart frame=0
    13:15:50  xruns=0  deficit=0          snaps=0
    13:16:25  xruns=4  deficit=0          snaps=0

A deficit of 484136464 frames is about 2.8 hours of audio.

Three facts explain why only a Mac restart corrected it.

**The snap cannot correct a large divergence.** `kSnapThresholdFrames` is 512
(`JackBridge.cpp:155`). The snap corrects small drift. At a deficit six orders
of magnitude above the threshold it fired 52 times in one window and never
converged. There is no path that stops snapping and re-anchors instead.

**The pi cannot repair a Mac anchor.** `syncMode` is 1, so the daemon owns the
timeline. A full restart of the pi jack stack was measured to change nothing:
the ring kept slipping at 83.3 messages per second per channel, the same rate
before and after. The pi was transporting audio correctly against a Mac
timeline that was hours out.

**The only re-anchor is process start.** The recovery is one log line, and it
comes from a fresh daemon.

`JB_OFF_RESYNC_REQUEST` is annotated "app to driver; reserved for automatic
re-anchor" and its value is 0. The mechanism was reserved and never built.

This is the same shape as the `driverStatus` trap in CLAUDE.md rule 6: Mac-side
shared-memory state with no way back except a restart.

**This blocks the whole plan, which is why paragraph 3.0 comes first.** Every
window in this plan compares counters before and after a change. A timeline
that can silently diverge, and that reports itself healthy while diverged, can
invalidate any window without telling you. The 2026-08-30 session lost two
round-trip measurements to exactly this.

## 3. The order of the work

Seven phases, numbered from 0. Do them in this sequence. Measure after each phase, and write the
result in `docs/JITTER.md` before you start the next phase.

Hold `-l 4` and `-g 1024` through phase 1 to phase 5. That pair is stable.
Phase 6 is the only phase that changes it.

### 3.0 Phase 0 — the timeline must recover by itself

Paragraph 2.9 gives the fault. Do this before any measurement, because a
diverged timeline reports itself healthy and silently voids a window.

**Step 0.1 — rebuild and reinstall the engine.**

The installed binary is not this source tree (paragraph 2.2). Run `just
reload`. Confirm that the daemon's startup latency line matches the constants
in the header before you go further.

**Step 0.2 — make the divergence visible.**

`deficit` and `snaps` appear only in an `os_log` line. `just shm` does not
print them, and neither does the menu bar, so a stack can be hours out of
anchor and still show green.

Publish both to shared memory, in the protocol 10 change of paragraph 3.2, and
print them in `jbdump`. A non-zero `snaps` count in steady state is the signal
that the timeline is not holding.

**Step 0.3 — bound the snap.**

The snap corrects drift up to `kSnapThresholdFrames`. Give it an upper bound
as well. When the deficit stays above a limit for several consecutive windows,
stop snapping and re-anchor, in the way that a fresh daemon does at startup.

The daemon already has the code: it runs `re-anchored after HAL restart` on
each start. The work is to reach that path without a process restart.

**Step 0.4 — use the field that exists, or delete it.**

`JB_OFF_RESYNC_REQUEST` is reserved for exactly this and is unused. Either
drive step 0.3 through it, so the app and the driver can also ask for a
re-anchor, or remove the field and its comment. A reserved field that no code
writes is a claim that the feature exists.

**Acceptance.** Force a divergence: hold the pi's service down long enough for
the anchor to drift, then bring it back. The stack returns to `deficit` near 0
and `snaps` 0 with no restart of any Mac process. The DAW keeps the device,
because nothing bounced.

### 3.1 Phase 1 — the Mac owns every pi parameter

This phase makes every later measurement repeatable. Do it first.

**Step 1.1 — extend the pi environment surface.**

Each tuning value in the pi helpers becomes a `JACKBRIDGE_*` variable with the
present constant as its default. The scripts keep working with no environment
file, so the change is safe on its own.

| Variable | Applies | Script | Default |
|----------|---------|--------|---------|
| `JACKBRIDGE_NET_LATENCY` | netadapter `-l` | `jackbridge-pi-up` | 4 |
| `JACKBRIDGE_NET_RING` | netadapter `-g` | `jackbridge-pi-up` | 1024 |
| `JACKBRIDGE_RT_NAPI` | `chrt -f` on `napi/$IFACE-*` | `jackbridge-napi-rt` | 72 |
| `JACKBRIDGE_RT_IRQ` | `chrt -f` on `irq/<n>-$IFACE` | `jackbridge-napi-rt` | 73 |
| `JACKBRIDGE_CPU_NET` | `taskset` for NAPI, and `smp_affinity_list` for the eth0 interrupt | `jackbridge-napi-rt` | empty, meaning no change |
| `JACKBRIDGE_CPU_DSP` | `taskset` for mod-host | `jackbridge-napi-rt` | empty, meaning no change |
| `JACKBRIDGE_GOVERNOR` | `scaling_governor` | `jackbridge-napi-rt` | empty, meaning no change |
| `JACKBRIDGE_QDISC` | `tc qdisc replace` on the interface | `jackbridge-napi-rt` | empty, meaning no change |
| `JACKBRIDGE_EEE` | `ethtool --set-eee` | `jackbridge-napi-rt` | empty, meaning no change |
| `JACKBRIDGE_NIC_RING` | `ethtool -G rx tx` | `jackbridge-napi-rt` | empty, meaning no change |
| `JACKBRIDGE_NIC_OFFLOAD` | `ethtool -K gro lro` | `jackbridge-napi-rt` | empty, meaning no change |

The defaults are the present behaviour. An empty value means the script does
not touch that setting. This keeps phase 1 free of any timing change, so a
measurement before it and a measurement after it must agree.

`jackbridge-napi-rt` already runs as root through the `+` prefix, and it
already reads `/run/jackbridge.iface`. It is the correct place for all of
these. Keep every new operation non-fatal, in the manner of the present
`ethtool -C` block: a driver that refuses a knob must not fail the unit.

**Step 1.2 — the raise-together rule.**

`jackbridge-pi-up` already warns when `NET_RING / 2` is not more than
`NET_LATENCY * period`. Keep that warning. Add the same check on the Mac, in
the Settings window, so that the user cannot write an invalid pair.

    NET_RING / 2  >  NET_LATENCY * period

**Step 1.3 — `push_pi_config` in `jackbridge-ctl`.**

Read each key from `config.plist`. Write `/etc/default/jackbridge` on the pi
over SSH (Secure Shell). Call it before `systemctl start`, in both
`pi_service start` and `cmd_pi_start`. `pi_ssh` already exists at
`jackbridge/tools/jackbridge-ctl:70`.

Treat a failure as a warning. An unreachable pi must not block a Mac restart,
because `pi_service` is best-effort by design.

Verify that the pi `sudoers` file permits `pistomp` to run `sudo tee` without
a password. This was assumed and never tested.

**Step 1.4 — the Settings window.**

Add a **Pi tuning** section. Each key gets a control. Group them:

- Network loop: L and G.
- Realtime: netadapter priority, NAPI priority.
- CPU: network CPU list, plugin CPU list.
- Interface: governor, queue discipline, Energy Efficient Ethernet, NIC ring
  size, offloads.

`ConfigStore` needs each key in `defaults`, in `populate()`, and in `apply()`.
Follow the pattern that `JitterFrames` already uses at
`SettingsWindowController.swift:208` and `:285`.

**Step 1.5 — the three things called "jackbridge".**

The name is ambiguous on the pi, and only the packet path is tuned:

| Name | What it is | Tuned here |
|------|-----------|-----------|
| `napi/$IFACE-*` | the packet polling kthreads | yes |
| `irq/<n>-$IFACE` | the interrupt thread | yes |
| netadapter | two SCHED_FIFO threads **inside** the `jackd` process | no |
| `pi-stomp-jackbridge.service` | the unit. Its main process is `jackbridge-xrun-watcher`, a python3 script | no |

The unit has no realtime thread of its own. It loads netadapter into the stock
jackd, then runs the watcher, which is telemetry.

The netadapter threads are deliberately left alone. Paragraph 3.4 gives the
reason. This also removes a practical problem: **every thread in the jackd
process has the name `jackd`**, so a script cannot find the netadapter threads
by name, and a rule such as "the jackd threads at priority 70" would also
match threads that are not the netadapter's.

**What this phase does not cover.** `isolcpus`, `nohz_full` and `rcu_nocbs`
are kernel command lines in `/boot/firmware/cmdline.txt`. They need a reboot,
they are not environment variables, and the Mac cannot own them. This plan
does not use them.

Pinning and isolation are not the same thing, and the difference is the reason
for that decision. `taskset` says "this thread runs on this CPU". `isolcpus`
says "the scheduler may place nothing on this CPU at all", which removes the
core from the general pool and gives the rest of the system three cores for
every other task. That is a large, permanent cost for a benefit we have not
measured, and it cannot be changed from the Mac or undone without a reboot.
Pin the network work to the last core, and let the kernel keep using that core
for whatever else needs it.

**Acceptance.** Change L in the Settings window. Click Apply. Restart. The
file `/etc/default/jackbridge` on the pi holds the new value. Reimage the pi,
start from the Mac, and the file returns with the correct values.

### 3.2 Phase 2 — the latency number becomes true

This is the protocol 10 change. It removes the 2.67 ms alignment error from
paragraph 2.2, and it makes every later latency figure trustworthy.

**Step 2.1 — shared memory fields.**

    #define JB_OFF_NET_LATENCY_CYCLES  (0x1d8)
    #define JB_OFF_NET_RING_FRAMES     (0x1e0)

Both fit before `JB_OFF_DEVICE_NAME` at `0x200`. Add `static_assert` guards in
the style of the guards at `JackBridge.h:320`.

**Bump `JACKBRIDGE_PROTOCOL_VERSION` from 9 to 10.**

**Step 2.2 — the latency function takes L and G.**

Add a four-argument form of `jb_one_way_latency_frames()`. Keep the
two-argument form, which calls the four-argument form with the compile-time
constants. The header is C++ only, so an overload is safe: it has
`static_assert` and `std::atomic`, it has no `extern "C"`, and the one C file
in the tree, `jackbridge/tools/rmshm.c`, does not include it.

**Step 2.3 — the daemon publishes, the driver reads.**

The daemon reads `NetLatency` and `NetRing` from `config.plist` and writes
both to shared memory, in the manner of `shmJitterFrames`. The driver reads
both in `_UpdateAdvertisedLatency()` at `SA_Device.cpp:1884`. A zero value
means the daemon has not written yet, so the driver uses the compile-time
constant. A new driver with an old daemon must still advertise a sensible
number.

**Step 2.4 — `jbdump` prints the new fields.**

An earlier plan named `jbdump` as a caller of the latency function. It is not.
`jackbridge/tools/jbdump.cpp` does not call that function, and it does not
print `JitterFrames` either. Add all three fields, or the acceptance test for
this phase cannot run. `just shm` builds and runs this tool.

**Step 2.5 — correct one stale comment.**

`JackBridge.h:225` states that `-l` is left unset in `jackbridge-pi-up`, so
the netadapter default runs. That is no longer true.
`jackbridge-pi-up:83` passes `-l` explicitly. Correct the comment in the same
change, because the next reader will otherwise trust it.

**Acceptance.** `just shm` prints `NET_LATENCY` and `NET_RING`. The daemon log
line and the driver log line report the same one-way figure. With L=4 the
figure is 1106 frames.

### 3.3 Phase 3 — settle the workgroup question

Paragraph 2.8 gives the difficulty. The workgroup axis moves the result by a
factor of 2. The DAW buffer size moves it by a factor of 5, and the driver
does not let a host choose it. A test of three workgroup values against an
uncontrolled larger term does not settle anything.

So this phase has two steps, and the first is not optional.

**Step 3.1 — implement the buffer size selectors.**

Add `kAudioDevicePropertyBufferFrameSize` and
`kAudioDevicePropertyBufferFrameSizeRange` to `SA_Device.cpp`. `JITTER.md`
calls this untried and cheap, and it is still untried. Until it exists, the
DAW gets 128 frames and no host can ask for 64.

This step has value on its own, separate from the workgroup question. It is
probably the single largest available reduction in Mac-side xruns.

**Step 3.2 — test the three workgroup values.**

With the buffer pinned at 64 frames, and with L=4 and G=1024, measure each of
`backend`, `hal`, and `none`. Use two 5 minute windows for each value. Use the
counter in paragraph 4.1 and the Mac xrun counters together.

Write the winner into `jackbridge/installer/config.plist`. The present default
is `backend`, and the one matched measurement in `JITTER.md` favours `hal` by
a factor of 1.7. Expect to change the default. Record the decision in
`JITTER.md` so that this axis is never tested again.

**Acceptance.** One workgroup value is chosen, with a measurement at a pinned
buffer size behind it. `config.plist` holds it.

### 3.4 Phase 4 — correct the priority of the packet path

Paragraph 2.5 gives the fault. The eth0 interrupt thread is at priority 50 and
the NAPI threads are at 60. mod-host is at 70, so mod-host preempts both. The
work that delivers a packet ranks below the work that is waiting for it.

**Step 4.1 — raise the packet path above mod-host.** *(done — these are now
the defaults in `config.plist` and in `jackbridge-napi-rt`.)*

    JACKBRIDGE_RT_IRQ  = 73
    JACKBRIDGE_RT_NAPI = 72

Observed ladder on a Pi 5, after the change:

    90  irq/<n>-dw_axi_dmac_platform   the DAC's DMA (pi-Stomp owns it)
    80  ttymidi (serial reader)        pi-Stomp owns it
    75  jackd                          backend + graph dispatch (-P 75)
    73  irq/<n>-eth*                   JACKBRIDGE_RT_IRQ
    72  napi/eth0-*                    JACKBRIDGE_RT_NAPI
    70  mod-host, ttymidi (jack)       the plugin graph

IRQ one step above NAPI because the interrupt is what schedules the poll.

**Step 4.1a — a prerequisite that was silently failing.** `iface_irqs()` looked
only at `/sys/class/net/$IFACE/device/{msi_irqs,irq}`. On a Pi 5 the onboard
port is an OF platform device (`raspberrypi,rp1-gem` / `cdns,macb`) with
neither attribute, so the lookup returned nothing, the loop body never ran, and
every log line lived *inside* that loop — `JACKBRIDGE_RT_IRQ` was accepted on
the Mac, pushed to `/etc/default/jackbridge`, and applied to nothing, without a
word. It now falls back to the last column of `/proc/interrupts` (the one name
in the system that reads `eth0` rather than the driver's unexpanded `eth%d`)
and reports the empty case. Any `NetRtIrq` measurement taken before this fix
measured nothing.

This change is nearly free, and that is the reason to make it first. An
interrupt handler and a NAPI poll each do a small, bounded amount of work and
then yield. Raising them takes almost no processor time away from mod-host. It
removes a preemption, not a share of the CPU.

**Step 4.2 — do not raise the netadapter above mod-host.**

An earlier plan proposed netadapter at 74, above mod-host. Do not do this.

The netadapter and mod-host are both large consumers of processor time. If
netadapter preempts mod-host, mod-host misses the graph deadline instead, the
pi jackd reports an xrun, and the user hears the same fault from a different
cause. The change moves the fault; it does not correct it.

The packet path is different, because it is small. This is the whole
distinction, and it is why step 4.1 is worth doing and step 4.2 is not.

If step 4.1 and phase 5 do not bring the counters down, and the evidence then
shows the netadapter itself starving, revisit this with a measurement. Do not
revisit it on the argument alone.

**Step 4.3 — put the packet path on the last core.**

    JACKBRIDGE_CPU_NET = 3        # the last core; 4 cores today, so index 3
    JACKBRIDGE_CPU_DSP = 0-2

This must set three things together, or the path is split across cores:

    taskset  on each napi/$IFACE-* thread
    /proc/irq/<n>/smp_affinity_list   for the eth0 interrupt
    taskset  on the mod-host threads, to keep them off that core

The interrupt affinity is the part that is easy to miss. If NAPI is pinned to
the last core and the interrupt still arrives on CPU0, every packet costs a
handoff between cores. Paragraph 2.7 shows the present scattering: the
interrupt thread on CPU0, one NAPI thread on CPU2, the other sharing CPU1 with
mod-host.

`jackbridge-napi-rt` should derive the default from `nproc - 1` rather than
hold the number 3, so that a pi with a different core count still uses its last
core. It already reads `/run/jackbridge.iface`, so it can read the interrupt
number for that interface out of `/proc/interrupts`.

**A conflicting theory, on the record.**

The comment in `jackbridge/pi/bin/jackbridge-napi-rt` states that NAPI is set
to 60 on purpose, so that "the audio threads finish their cycle first and then
NAPI delivers the next batch". Step 4.1 asserts the opposite.

Neither has been measured. The argument for 60 is not empty: with `-l 4` the
slave has four cycles of slack, so a packet does not have to arrive within the
cycle that asked for it. The argument against it is that a delay of one
plugin quantum is not bounded by anything, and four cycles is 5.33 ms, which
the measured worst-case round trip already approaches.

Measure both. If 72 is worse than 60, write that in `docs/JITTER.md` and
restore 60, and correct the comment either way, because it currently states a
design decision as though it were established.

**Step 4.4 — measure the steps separately.**

Step 4.1 and step 4.3 are two changes. Measure after each. A single combined
window cannot say whether priority or placement gave the result.

**Acceptance.** The per-channel count decreases. The pi ALSA xruns stay at 0,
which is the test that step 4.2 is not quietly happening anyway.

### 3.5 Phase 5 — decrease the jitter

These are the remaining interface and power tasks. Each is now a key in
`config.plist`,
so each is one Apply and one restart. Do one at a time. Measure after each.

| Task | Key | Value |
|------|-----|-------|
| Power supply | none | Use the correct supply, a short cable, no hub. |
| CPU frequency | `JACKBRIDGE_GOVERNOR` | `performance` |
| Queue discipline | `JACKBRIDGE_QDISC` | `pfifo_fast` |
| Energy Efficient Ethernet | `JACKBRIDGE_EEE` | `off` |
| NIC ring size | `JACKBRIDGE_NIC_RING` | 128 |
| Offloads | `JACKBRIDGE_NIC_OFFLOAD` | `off` |
| Thunderbolt adapter | none | Physical change on the Mac. |

Do the power supply first. A low voltage decreases the CPU frequency with no
message to the audio software, so it corrupts every other measurement. The
present reading is not valid, for the reason in paragraph 2.6. Run one hour of
audio, then read `vcgencmd get_throttled`. It must be `0x0`.

`en7` is a USB network adapter, and USB moves data on a fixed schedule. Keep the USB adapter if the
maximum round trip time does not decrease by more than 1 ms.

The user's fifth point is correct: both machines have ample computing power,
so the present jitter is not a capacity limit. It is a scheduling and
interface-configuration fault. Every task in this phase treats it as one.

**Acceptance.** The maximum round trip time is less than the loop cushion. The
cushion is `L * 1.333 ms`, which is 5.33 ms at L=4.

### 3.6 Phase 6 — decrease the cushion

Only now. Each cycle of cushion costs monitoring delay, so the smallest stable
pair is the goal, but a pair that is too small returns the clicks.

Change L and G in `config.plist` on the Mac. Never on the pi.

    G / 2  >  L * period            period = 64

    -l 4, -g 1024  ->  512 > 256    the present pair
    -l 3, -g 1024  ->  512 > 192    valid
    -l 2, -g  512  ->  256 > 128    valid, but recorded as unstable under load

Decrease L by 1. Measure one 5 minute window. Repeat until the counters
increase. Then return one step.

Phase 2 removed the need for the old warning here: the constants no longer
have to be edited by hand, because the daemon publishes the live values. This
is the reason phase 2 comes before phase 6.

**Acceptance.** The counters stay at 0 for 30 minutes with mod-host under
load. A person hears no clicks in 10 minutes of playback.

## 4. How to measure

### 4.1 The counter that counts the clicks

```sh
ssh pistomp@pistomp.local \
    'sudo journalctl -u jack --since "-5 min" --no-pager \
     | grep -oE "producer too slow|consumer too slow" | sort | uniq -c'
```

Divide `producer too slow` by 2, because the link carries 2 channels from the
Mac to the pi. Divide `consumer too slow` by 4, because it carries 4 channels
from the pi to the Mac. The two results must agree. They were measured to
agree within 0.3 percent, which is why one lost cycle, and not one direction,
is the fault. `JackNetAdapter::Process` does `Read`, then `PushAndPull`, then
`Write` in one sequence, so one late cycle damages both rings together.

Caution: confirm that the audio streams before each window. Run `just shm`.
`driverStatus` must be `STARTED`, and `halOutputWriteHead` must increase. If
the head does not increase, the window is not valid.

Caution: confirm that the timeline holds, before **and** after each window.
`deficit` must be near 0 and `snaps` must be 0 in the daemon health line. A
diverged timeline reports `STARTED`, no fault, a live heartbeat and 6 of 6
ports, and it makes the counters in this paragraph meaningless. Paragraph 2.9
gives the case. Until step 0.2 is done, read them with:

```sh
/usr/bin/log show --last 2m --predicate 'subsystem == "com.treefallsound.companion"' \
    --info | grep 'health xruns'
```

### 4.2 The round trip time

```sh
ping6 -c 60 -i 0.1 'fe80::<pi eth0>%en7'
```

Use the maximum value and the standard deviation. Do not use the average
value, because the average stays good while the clicks continue.

`ping pistomp.local` gives 100 percent loss. mDNS returns the WiFi address.
The audio link is the direct cable, so use the link-local address.

### 4.3 The window

Use two windows of 5 minutes, one after the other, with the change between
them. Do not compare a new window with a window from another day. A recorded
count moved from 72 to 31 with no change to the system.

Measure with mod-host under load. A quiet pi does not show the fault.

## 5. The acceptance criteria

- The per-channel count is less than 5 in each 5 minute window.
- The maximum round trip time is less than the cushion, `L * 1.333 ms`.
- `vcgencmd get_throttled` gives `0x0` after one hour of audio.
- The pi ALSA xruns stay at 0.
- `/etc/default/jackbridge` is written only by `jackbridge-ctl`.
- The timeline recovers from a forced divergence with no process restart.
- A reimage of the pi costs no configuration.
- A person hears no clicks in 10 minutes of playback with mod-host under load.

The last criterion decides. The Mac xrun counters stay at 0 while the clicks
continue, so no Mac-side counter can replace a person listening.

## 6. What we do not know

- No task in phase 4 or phase 5 has a measured result. The 2026-08-30 window
  for the priority change was cancelled before it completed.
- We do not know whether raising the packet path above mod-host helps or
  harms. Paragraph 3.4 records both theories and measures them.
- We do not know the priority of the mod-host DSP threads under load. The main
  process is at 65; an earlier plan recorded ten DSP threads at 70. If any of
  them is at 70 it sits between NAPI and the netadapter, which changes nothing
  about step 4.1 but is worth knowing.
- We do not know the delay of the netJACK2 packets. Every measurement uses
  ICMP (Internet Control Message Protocol). The netadapter receives its
  packets on a realtime thread, and ICMP does not.
- We do not know what made the timeline diverge on 2026-08-30. Paragraph 2.9
  records the recovery, not the cause. The pi had a clock step of about 11
  hours while jackd was running, which is a candidate and is not proof; a pi
  jack restart afterwards did not clear the fault, so the divergence had
  already been taken by the Mac daemon and held there.
- We do not know the effect of the buffer size selectors in step 3.1, because
  they have never existed. The 5x figure in paragraph 2.8 is a comparison
  between two hosts' defaults, not a controlled test.

## 7. Implementation status

Updated 2026-08-30, after the first implementation pass. Everything below is
either code that exists in the working tree or a measurement that does not.
Nothing here is a measured result: no window in this plan has been run against
the new code yet.

### Built

**Phase 0 — timeline recovery.** `reanchor()` in `jackbridge/daemon/JackBridge.cpp`
is now the single re-anchor path, reached by three callers: the HAL-restart
transition (as before), a new `JB_OFF_RESYNC_REQUEST` nonce (step 0.4 — the
daemon reads the app's field alongside the driver, rather than the field being
deleted), and the automatic divergence recovery (step 0.3: `deltaMax` past
`kReanchorThresholdFrames` for `kReanchorWindows` consecutive windows stops
snapping and re-anchors). `deltaMax`, `snaps` and a re-anchor count are
published to shm and printed by `jbdump` (step 0.2).

**Phase 1 — Mac owns every pi parameter.** All eleven variables in the 3.1
table exist in `jackbridge/pi/bin/jackbridge-napi-rt` and `jackbridge-pi-up`,
with the present behaviour as the default and empty meaning "leave alone".
`push_pi_config` in `jackbridge/tools/jackbridge-ctl` writes
`/etc/default/jackbridge` from `config.plist` before every `systemctl start`,
in both `pi_service start` and `pi-start`. The Settings window has a **Pi
tuning** section, and refuses a loop pair that violates
`NetRing / 2 > NetLatency * period`. The `sudoers` question in step 1.3 is
answered: `pistomp` has `(ALL) NOPASSWD: ALL`, so `sudo tee` works.

**Phase 2 — the latency number.** Protocol 10. `NetLatency`/`NetRing` are
published at `0x1d8`/`0x1e0`, `jb_one_way_latency_frames()` has a
four-argument form, the daemon and the HAL both use the published pair, and
`jbdump` prints the pair and the one-way figure it produces. The stale comment
about `-l` being left unset is corrected. `JB_NET_LATENCY_CYCLES` is now 4,
matching what the pi runs and what `config.plist` ships.

**Phase 3 step 3.1 — buffer size selectors.** `kAudioDevicePropertyBufferFrameSize`
and `...BufferFrameSizeRange` are implemented in `SA_Device.cpp`, range
32..256 frames, defaulting to the JACK period the daemon published. A host can
now ask for 64.

### Not built, because it is a measurement

Phases 3.2, 4, 5 and 6 are windows, not code. Every knob they turn is already a
key in `config.plist`, so each is one Apply and one restart — which was the
point of doing phase 1 first. The order in section 3 still stands, and the
results still go in `docs/JITTER.md` before the next phase starts.

The one thing that must happen before any of them: **rebuild and install the
engine** (step 0.1, `just reload`). Protocol 10 will not attach to a region a
protocol-9 binary published, so the driver, the daemon and `jbdump` have to
move together — which is exactly the forced clean rebuild the version bump is
for.
