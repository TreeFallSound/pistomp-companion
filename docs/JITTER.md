# Jitter / crackle investigation

## Status update 2026-06-02 — root cause localized

The "still to try" list below (hops 8/9 — daemon shm read/write path) is
**not** the cause. New evidence below points to **Mac jackd's netJACK2
master client (`pistomp`) being preempted under Mac CPU load**.

Evidence:

1. **Reproducer.** Three-finger swipe between desktops on macOS (heavy
   WindowServer load) reliably produces clicks. CPU pressure on the Mac
   is the trigger.
2. **HAL is fine.** Driver-side health log shows `nearMiss=0` and
   `leadJitter ≈ 1 frame/cycle` even during the swipe. CoreAudio IOProc
   is not being starved.
3. **`kAudioDevicePropertySafetyOffset` doesn't help.** Tested at 192,
   1024, and 4096 frames: identical click rate. (See "the SafetyOffset
   experiment" below.)
4. **xrun client identity.** `/tmp/com.jackbridge.jackd.err.log` shows
   every xrun is `client = pistomp was not finished, state = Running`
   immediately followed by `client = JackBridge #1 was not finished,
   state = Triggered`. Our daemon never even starts the failed cycle
   — it's queued behind `pistomp`, which blew its budget.
5. **Pi-side jackd shows 0 xruns** through all of this. The Pi is
   processing its half cleanly; the failure is entirely Mac-internal to
   jackd's netJACK2 master thread.
6. **jackd's RT threads are PRI 97 (time-constraint policy)** but
   jackd 1.9.22 does not join the device workgroup. Workgroup-joined
   threads (our daemon does join) get cooperative scheduling protection
   against WindowServer pressure that non-joined RT threads do not.

So the actual graph and failure site:

```
Mac jackd cycle: coreaudio backend → pistomp (netJACK2) → JackBridge #1 (us)
                                         ↑
                                  deadline missed here
                                  (under Mac CPU load)
```

Implications:

- Hops 8/9 in the table below are **eliminated**: our daemon never runs
  in the failed cycles, so it cannot be writing torn frames.
- Hops 1–7 remain eliminated under empty pedalboard.
- Hop 10 (HAL IOProc) was already eliminated; SafetyOffset experiment
  reconfirmed.
- The remaining failure mode is **inside jackd 1.9.22**, specifically
  its netJACK2 master client thread on macOS under preemption pressure.

### The SafetyOffset experiment (kept here as a record)

We added `kAudioDevicePropertySafetyOffset = JitterFrames` to the HAL,
expecting that giving CoreAudio more producer-side lead would absorb the
swipe-induced bunching. Result: zero observable effect on click rate at
any value 0 → 4096. Driver `nearMiss` counter stays at 0 across the
range, confirming HAL scheduling is not the bottleneck. Will be torn
back out in a follow-up commit; preserved here so we don't redo the
experiment.

### What to try next

Things that *might* help, all targeting the jackd / netJACK2 layer
rather than JackBridge:

1. **Bump `pistomp` (netJACK2 master) RT priority above 75.** jackd
   currently runs all clients at the `-P 75` budget; netJACK2's master
   thread might benefit from being higher. Or split: keep the backend at
   75, give netJACK2 master 80.
2. **Pin `pistomp` to an E-core or to a specific P-core.** Avoids
   contention with WindowServer, which prefers P-cores.
3. **Increase Mac PeriodFrames from 64 to 128.** Doubles netJACK2
   master's budget per cycle. Costs latency (one more T_mj = 1.33 ms);
   may be acceptable if it eliminates clicks.
4. **Try jackd 1.9.23+ if it adds workgroup support.** Speculative —
   needs source check.
5. **File an upstream jack2 issue** with the evidence above. The cause
   is jack2 not being workgroup-aware on Apple Silicon.

The text below (mental model and prior "still to try" list) is
preserved for context but **is no longer the right place to look**.

---

## What we've eliminated (with evidence)

### 1. Mac HAL "guarantee MISS" log — bogus check, removed
The old `outOverrun count=3750 maxAmount=<linear>` log was comparing
HAL-input-cursor against HAL-output-cursor, both written by HAL itself. It
measured the natural in/out sampleTime skew, not daemon shortfall. Removed
in `SA_Device.cpp` (jitter min/mean/max line is the surviving RT-spike
signal). Not a cause of clicks, but was misleading the investigation.

### 2. Mac IOProc wake jitter — substantially reduced
After workgroup membership work, steady-state jitter log shows
`inLead/outLead` tight at 54–73 around mean=63 across every 5-second
window. Earlier sessions showed `min=-282 / max=411` spike windows; those
are now absent in normal operation. The remaining `nframes=1776` event we
saw at 14:36:29 was a buffer-size renegotiation, not a stall.

### 3. Pi-side RT scheduling / load — not the dominant cause
- jackd RT thread is `SCHED_FIFO` prio 75. `net1` threads are FIFO 70.
  mod-host plugins are 65–70. CPU governor `performance`, all 4 cores at
  2.4 GHz, `throttled=0x0`.
- mod-host steady-state is ~28% CPU. Load avg ranges 1.6 to 3.2.
- With a **completely empty pedalboard** (load avg 1.61, mod-host idle):
  `journalctl -u jack --since "2 min ago" | grep -c "JackEngine::XRun"` = 0,
  `... too slow` = 0, `ringbuffer failure` = 0. **And clicks persist.**
  This rules out the pi as the click source under empty-board conditions.

### 4. Ethernet IRQ co-location with jackd RT thread (pi)
`irq/105-eth%d` was pinned to CPU 3 (same as jackd RT TID 504). Moved it
to CPU 0. xrun rate before: 12/60s. After: 10/60s. Effect within noise.
**Not the dominant cause.** The change is harmless to leave in place.

### 5. UDP packet loss / reordering on the wire
Paired `tcpdump` on `end0` (pi) and `en7` (Mac) for 30 seconds while clicks
were audible (`jackbridge/tools/netjack-loss-test.sh`):
- 21,802 cycles in the common window, both directions.
- 0 missing cycles, 0 out-of-order arrivals, 0 partial cycles.
- Cross-side: 0 packets seen on the sender side but not the receiver.
- Inter-arrival p99 ≈ 2.7ms, max ≈ 13ms — real jitter but no drops.
- pi `ip -s link show end0`: 0 RX errors, 0 fifo overruns over 21M packets.
- Mac `netstat -i`: 0 Ierrs, 0 Oerrs on en7.

The wire and both NIC stacks are clean.

### 6. Netadapter ring "producer/consumer too slow" events
These come from `JackAudioAdapter::PushAndPull`, the slip ring inside the
netadapter that bridges Mac CoreAudio's 48 kHz clock and pi ALSA hw:0's
48 kHz clock (they're independent crystals — small drift between them).
Default mode is "automatic adaptative" sizing: the ring **starts small and
doubles on each failure**, each doubling being audible. Journal evidence:

```
... ringbuffer failure... reset
Ringbuffer size = 1024 frames
... ringbuffer failure... reset
Ringbuffer size = 2048 frames
```

With an empty pedalboard, these events are **not currently firing**. They
were contributing to clicks under load but are not responsible for the
current symptom.

### 7. Netadapter graph xruns (mod-host + plugins blowing budget)
`JackEngine::XRun: client = effect_NN was not finished` events fire when
the LV2 chain + netadapter exceed the 64-frame period on the pi. With
plugins loaded these contributed to clicks. With empty pedalboard: 0
events in 2 minutes.

---

## Mental model of every place jitter can enter (unchanged)

End-to-end, audio leaves the pi's ADC, traverses both clock domains,
and ends up in the DAW. Each hop is a place variance can grow:

```
[ADC] → (1) jackd audio thread → (2) netadapter writer →
        (3) qdisc → (4) pi macb TX → (5) PHY / cable →
        (6) Mac NIC RX → (7) macOS network stack →
        (8) JackBridged reader → (9) shm + atomics →
        (10) HAL IOProc → CoreAudio → DAW
```

| # | Hop                                | Status now                                       |
|---|------------------------------------|--------------------------------------------------|
| 1 | jackd audio thread wake (pi)       | RT priorities sane; eliminated with empty board  |
| 2 | netadapter writer                  | priority OK; not contributing under empty board  |
| 3 | egress qdisc (pi)                  | fq_codel, no drops                               |
| 4 | pi macb TX completion              | 0 TX errors, 0 fifo overruns                     |
| 5 | PHY / cable                        | not a variance source                            |
| 6 | Mac NIC RX                         | 0 Ierrs on en7                                   |
| 7 | macOS network stack                | 0 packet drops cross-correlated                  |
| 8 | JackBridged reader thread          | **untested — see "still to try" #1**             |
| 9 | shm handoff                        | **untested — see "still to try" #1**             |
| 10| CoreAudio IOProc wake (Mac)        | tight jitter log; spike windows absent           |

Of these, hops 1–7 and 10 have direct evidence ruling them out (or
significantly reducing) under empty-pedalboard conditions. Hops 8 and 9
have not been instrumented.

---

## What's left to try (ordered)

### 1. Audit JackBridged daemon's shm read/write path (hop 8/9)
Highest prior given everything else is ruled out. Looking for:
- **TOCTOU on the producer-consumer cursor.** Can HAL read from a frame
  position the daemon hasn't fully written? Need acquire/release on the
  *write* head update, with HAL doing acquire on read.
- **Ring wraparound bugs in the memcpy.** Off-by-one at the boundary
  produces sample-level discontinuities. Audit the split-memcpy at wrap.
- **Channel-stride / interleaving with 4-in/2-out asymmetry.** The
  hardcoded `*2` and `8`-byte-per-frame literals (per CLAUDE.md) need
  to be correct on *both* the in and out sides; the asymmetry between
  2-channel and 4-channel rings is a likely site for a bug.
- **Partial-frame reads on cycle boundaries.** Does the HAL ever read a
  frame for which only one channel was written?

### 2. Audit HAL driver's ring read (also hop 8/9)
Same family of bugs on the consumer side: stride, wrap, partial frame.
The HAL also runs `BeginIOOperation` / `DoIOOperation` / `EndIOOperation`
in a specific order — ensure no frame state crosses those boundaries
in a way that could be observed mid-update.

### 3. Click-localization with a known signal
If audits 1+2 are inconclusive, send a 1 kHz pure sine from pi into the
DAW, record 60 s, look at the spectrogram and waveform:
- **Sample-level discontinuities** (single-sample steps) = ring/shm bug.
- **Periodic at exact cycle/buffer cadence** = wraparound bug.
- **Sub-sample artifacts / sidebands** = SRC issue (netadapter resampler).
- **Truly random** = daemon preemption or unlogged packet effect.

Click positions in the time domain also tell us the rate, which we can
cross-reference with frame counts.

### 4. Instrument the netadapter resampler ratio
If clicks correlate with SRC adjustments rather than per-cycle work, we
can prove it by logging the `JackLibSampleRateResampler` ratio per cycle
and looking for discrete jumps. Requires a small modification to jack2
or LD_PRELOAD'ing a shim. Lower prior than #1–#3.

### 5. Bump netadapter `-g 2048` (jitter tolerance / fewer ring doublings)
**Not currently relevant** for the empty-board click — under empty board
the adapter ring isn't failing. But if we re-introduce load and the
adapter ring resets come back, `-g 2048` pins the ring at 2048 frames
(~43 ms) from boot and avoids the audible doubling-during-learn phase.
One-line change in `jackbridge/pi/bin/jackbridge-pi-up` (`jack_load netadapter -i
"-C 2 -P 4 -g 2048"`).

---

## Diagnostics on hand

- `jackbridge/tools/netjack-loss-test.sh [duration]` — paired pcap on both ends,
  parses netjack headers, reports drops / gaps / out-of-order / jitter
  + cross-side reconciliation. Needs `sudo` on both ends. No deps beyond
  `python3` and `tcpdump`.
- HAL jitter log (5 s windows): `log stream --predicate 'subsystem ==
  "com.jackbridge"' | grep jitter`.
- Pi xrun categories:
  - `JackRingBuffer.*too slow` — netadapter slip ring failures.
  - `JackEngine::XRun: client = X was not finished` — graph xruns.
  - `ringbuffer failure... reset` followed by `Ringbuffer size = N` —
    netadapter ring doubled.
  - `JackEngine::XRun: client X finished after current callback` —
    client late, but recovered same cycle.

---

## FAQ

1. **Which direction has the clicks?** Playback (Mac → pi speakers)
   and recording (pi mic → Mac DAW) both have clicks.
  
2. **Are clicks correlated with anything observable?** They seem to be
   a bit more common when I'm doing a lot of other stuff on the Mac,
   but they happen even when I'm just sitting and playing back something
   on the pi-Stomp via JackRouter.

3. **Does the click rate change with sample rate or buffer size?** 
   Unknown so far.
