# Systems model: stocks, flows, and cushions

A top-down model of the whole audio path, from the pi's codec to the DAW.
It is derived from queueing and clock physics, **not** from the
implementation. Read it to decide what a cushion *should* be. Read
`docs/DEBUGGING.md` to find out what the running system *is*.

Where this model and the code disagree, one of the two is wrong. Section 8
lists the disagreements that are open.

Reference conditions for all arithmetic: `f_s` = 48000 Hz, `P` = 64 frames,
`L` = 4 cycles, `G` = 1024 frames, `J` = 128 frames, shm ring = 4096 frames,
`N` = 32…1024 frames.

| Frames | Time at 48 kHz |
|--------|----------------|
| 1 | 20.83 µs |
| 32 | 0.67 ms |
| 64 (P) | 1.333 ms |
| 128 | 2.667 ms |
| 256 | 5.333 ms |
| 512 | 10.67 ms |
| 1024 | 21.33 ms |
| 4096 | 85.33 ms |

---

## 1. The whole path

```
        pi-Stomp  (clock A)                     cable            Mac  (clock B)
 =====================================  ==================  =====================================

 RECORD  (pi mic -> REAPER)
 [ADC]->(S1)->(S2)->(S3)->(S4c)->(S5)->|(S6)|->(S7)->(S8)->(S9)->(S10i)->(S11i)->(S12)->[REAPER]
   ^      ^     ^     ^      ^     ^      ^     ^      ^     ^      ^       ^       ^
 codec  ALSA  jackd  mod-  SLIP   NIC   wire   NIC   cycle jackd   shm     HAL     DAW
 group  ring  port   host  RING   tx          rx    offset port    ring   block   queue
 delay        buf    graph (SRC)  +sock       +sock window  buf    (P->N)  (N)

 PLAYBACK  (REAPER -> pi speakers)
 [DAC]<-(T1)<-(T2)<-(T3)<-(T4p)<-(T5)<-|(S6)|<-(T7)<-(T8)<-(T9)<-(S10o)<-(S11o)<-(T12)<-[REAPER]

 ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 clock A drives S1,S2,S3,T1,T2,T3      clock B drives S8..S12 and T7..T12 AND the net cycle
 The ONE cross-clock bridge is S4c/T4p on the pi. It is the only SRC in the path.
```

### The record chain, with units

```
 [ADC, IQaudIO]        analog -> PCM. Group delay ~1 frame. Clock A.
 (S1)  ALSA CAPTURE RING            128 frames (period 64 x nperiods 2)
 (S2)  pi jackd CAPTURE PORT BUF     64 frames per port. Not a queue.
 (S3)  mod-host PLUGIN GRAPH        fixed delay. Not a queue.
 (S4c) netadapter CAPTURE SLIP RING G = 1024 frames, target G/2 = 512
       THE ONLY CROSS-CLOCK BRIDGE IN THE SYSTEM.
 (S5)  pi KERNEL UDP TX + NIC RING  packets / bytes
 (S6)  THE WIRE                     ~0.35 ms = 17 frames
 (S7)  Mac NIC RX + KERNEL UDP RX   packets / bytes
 (S8)  netmanager CYCLE OFFSET      L x P = 256 frames. A delay, not slack.
 (S9)  Mac jackd PORT BUFFER         64 frames per port. Not a queue.
 (S10i) shm INPUT RING              4096 frames. THE BLOCK-SIZE BOUNDARY.
 (S11i) COREAUDIO IO BUFFER         N frames (32..1024)
 (S12)  DAW INPUT / RECORD QUEUE    host chosen
 [REAPER]
```

The playback chain carries the same stages in reverse order. The two
directions are symmetric in structure. They are **not** symmetric in
measured jitter; see section 5.4.

---

## 2. The stocks

The governing rule:

> **A stock with equal average inflow and outflow, and with no controller, is
> a random walk.** The mean fill does not move. The variance grows without
> limit. The stock survives only because an external event re-phases the two
> flows, or because the walk is reflected at a boundary. If neither is true,
> the stock empties or fills. Only the time is in question.

Three stocks in this path have a controller:

- **S4c** — a PI controller on the resample ratio.
- **S8** — a phase latch that fixes the cycle offset at `L`.
- **S10** — a free-running cursor with a hazard snap.

Every other stock is an uncontrolled random walk, or a delay line with no
storage at all.

### S1 / T1 — the ALSA rings

Capacity `period_size x nperiods` = 128 frames. Inflow is the codec DMA on
clock A. Outflow is the pi jackd cycle, which wakes on the ALSA period
interrupt. In and out are phase-locked, so the stock absorbs only the
scheduling jitter of the pi jackd thread. The whole budget is 2 periods.

### S2 / T2 / S9 / T9 — the jackd port buffers

**Not stocks.** Each is a rendezvous slot of exactly `P` frames. jackd fills
it, the graph consumes it, and the same memory is reused next cycle.
Overflow and underflow are impossible by construction: a late client
produces a graph xrun and the cycle is abandoned.

Their only systems role is to force every stage in the graph to the same
block size.

### S3 / T3 — the mod-host plugin graph

**Not a queue.** It is a delay line. Inflow equals outflow every cycle.

Its failure mode is not overflow. It is deadline miss. If the graph needs
longer than `P/f_s`, jackd declares a graph xrun and the whole cycle is
lost, the netadapter's cycle with it. **mod-host CPU load therefore couples
directly to the network cycle, with no pi-side absorption.**

mod-host declares no LV2 latency and implements no JACK latency callback, so
the wet pair carries an unreported delay that the dry pair does not. Summing
them in the DAW gives comb filtering, not a delay.

### S4c / T4p — the netadapter slip rings

Capacity `G`. Target fill `G/2`. The PI controller reads the ring error and
outputs the resample ratio `r`.

**The load-bearing asymmetry: the ratio is computed from the capture ring
error only.** The playback ring runs on `1/r` and has no controller of its
own. A disturbance that affects only the playback direction is therefore
uncontrolled.

Without the controller, at 40 ppm relative drift = 1.92 frames/s, a
512-frame cushion lasts **267 seconds** and then fails permanently. That is
the impossibility result of `docs/CLOCK_WARS.md` in one number.

On failure the adapter resets **both** rings and discards the resampler
filter state. The reset is a policy, not a physical necessity. See section
6.2.

### S5 / T5 / S7 / T7 — socket buffers and NIC rings

Capacity is kernel default, hundreds of KB, against ~1 KB per cycle of
offered load. **Capacity is not the hazard here. Service time is.** NAPI
runs in softirq context, and an FF thread preempts softirq. A late-serviced
buffer is the failure; a full one is not.

### S6 — the wire

Bandwidth-delay product ~44 KB against ~1 KB of payload. Measured loss is
0.0 % on a direct cable. **The wire is not the problem. The endpoints are.**

### S8 — the netmanager cycle-offset window

`L x P` frames. **This is a delay, not a buffer with slack.** The master
latches the offset when the received cycle number reaches `L` behind the
sent one, then holds it. After the latch, `L` is the entire budget for one
round trip.

When the reply is late, the master blocks in `recv()` past its deadline and
**the Mac jackd graph xruns**. The daemon runs downstream of the master in
the graph, so its xruns are derivative. Do not read them as an independent
fault.

### S10i / S10o — the shm rings

Capacity 4096 frames. Inflow is the daemon at `P` frames per jackd cycle.
Outflow is the HAL at `N` frames per IO cycle. Both are on clock B.

**Rates are equal. There is no drift across this boundary, ever.** That is
the whole payoff of Config B. Phases are not locked and the block sizes
differ, so the fill is a deterministic sawtooth of amplitude `max(N,P)`,
plus a random walk driven by the scheduling jitter of the two threads.

Capacity is not the constraint. The geometric condition is
`ring > 2 x max(N,P) + J`, which at `N` = 1024 and `J` = 128 uses 2176 of
4096 frames.

### S11 — the CoreAudio IO buffer

`N` frames, host-selected. **A pure cadence, not a stock.** But it sets the
size of the stock upstream of it, which is why `N` matters far beyond its
own latency. At `N` = 1024 a single missed IO proc is a 21.33 ms hole.

### S12 / T12 — DAW buffering

It absorbs the DAW's own plugin-load spikes and nothing else. **It cannot
absorb any upstream fault**, because by the time the frames reach it the gap
is already in the samples.

---

## 3. The cadence map

There are exactly **two free-running sample clocks**. Everything else in the
audio path derives from one of them.

| # | Source | Origin | Free-running? |
|---|--------|--------|---------------|
| C1 | pi codec clock **A** | IQaudIO codec, BCM I2S PLL, pi crystal | **Yes.** No VCXO exists on this board. |
| C2 | Mac audio clock **B** | the crystal behind `ClockDeviceUID` | **Yes.** |
| C3 | pi jackd cycle | ALSA period interrupt from C1 | No |
| C4 | Mac jackd cycle | CoreAudio backend interrupt from C2 | No |
| C5 | netJACK cycle | the master runs inside C4 | No |
| C6 | HAL IO cadence | coreaudiod timer, anchored to the daemon | No in rate. **Yes in phase.** |
| C7 | DAW thread | called from C6 | No |
| C8 | pi NAPI / softirq / IRQ | the NIC interrupt, plus coalescing | **Yes.** Fully asynchronous. |

### Boundary analysis

| # | Boundary | Rate-locked? | Phase-locked? | Cushion needed |
|---|----------|--------------|---------------|----------------|
| B1 | codec -> pi ALSA | Yes | Yes | 1 period, for jackd wake jitter |
| B2 | pi ALSA -> pi jackd | Yes | Yes | 0 |
| B3 | pi jackd graph internal | Yes | Yes | 0 |
| B4 | **pi jackd -> net cycle** | **NO** | **NO** | **Unbounded without a controller** |
| B5 | netadapter -> pi NIC | No | No | absorbed inside `L` |
| B8 | **net round trip, master `recv`** | Yes | Latched, then rigid | `L x P` > worst-case turnaround |
| B9 | netmanager -> Mac jackd | Yes | **Yes** | **0** |
| B10 | Mac jackd -> daemon | Yes | **Yes** | **0** |
| B11 | **daemon -> HAL across shm** | **Yes** | **NO** | `N + P - gcd(N,P)`, plus jitter |
| B12 | HAL -> DAW | Yes | Yes | 0 |

The rule that follows:

> A boundary that is rate-locked **and** phase-locked needs zero cushion.
> A boundary that is rate-locked but **not** phase-locked needs a cushion
> equal to the worst-case phase excursion. That cushion is permanent, but it
> does not grow.
> A boundary that is **neither** needs a controller or an infinite buffer.
> There is no third option. This is why the resampler exists.

**No drift term belongs in the shm cushion.** Both sides share clock B.
A drift term there pays latency for a hazard that does not exist.

---

## 4. Pause and stall taxonomy

Absorption times use the nominal fills of section 2.

| # | Event | First absorber | Absorbs for | Result on failure |
|---|-------|----------------|-------------|-------------------|
| E1 | pi CPU contention | S1, 1 spare period | 1.33 ms | pi graph xrun; the netadapter cycle goes with it |
| E2 | pi DSP overrun | **none** — S3 is a delay line | **0 ms** | Immediate graph xrun, both directions at once |
| E3 | NAPI / softirq preemption | S8's `L` window | 5.33 ms | Mac jackd xrun. **And** T4p starves with no controller — the playback click no counter records |
| E4 | Interrupt coalescing | S8 | fixed 98 µs | Not a failure. 1.8 % of the loop budget, permanently spent |
| E5 | Packet loss | **nothing** | 0 | 1.33 ms missing. Measured loss is 0.0 %, so not the live failure |
| E6 | Reordering / late sub-cycle packet | nothing usable | 0 | The cycle is discarded |
| E7 | One lost netJACK cycle | `G/2` on the pi, S8 on the Mac | 10.67 / 5.33 ms | Ring reset, resampler state loss, click, plus settling |
| E8 | netadapter ring reset | nothing downstream | 0 | A guaranteed discontinuity. **Policy-generated**, not physical |
| E9 | Mac jackd late | `J` at S10 | 2.67 ms | Hole of up to `max(N,P)` frames |
| E10 | coreaudiod wake jitter | `J` at S10 | 2.67 ms | Same, in the other cursor |
| E11 | DAW late | S12 | host-chosen | Hole of `N` — up to 21.33 ms at `N` = 1024 |
| E12 | Buffer-size renegotiation | nothing | 0 | Geometry change; cursors re-seed. Make it rare, not silent |
| E13 | Cable replug | nothing in the audio path | 0 | Seconds of outage. **Must self-heal without bouncing the stack** |
| E14 | pi reboot / pedal on after the Mac | nothing | 0 | Tens of seconds. Repair with a retry loop, not a stack restart |
| E15 | daemon restart | nothing | 0 | A gap, then normal service. The HAL must not conclude the device is dead |
| E16 | coreaudiod restart | nothing | 0 | **The most expensive failure in the table, measured in user actions** |
| E17 | Clock A vs B drift, ±40 ppm | S4c/T4p via the PI controller | 267 s without it; indefinitely with it | The one failure the design has genuinely solved |

Two structural observations:

1. **Cushion exists in only three places in the whole path**: the ALSA ring
   (1.33 ms), the netadapter slip rings (10.67 ms), and the shm `J`
   (2.67 ms). Everywhere else is a rendezvous slot or a delay line. A stall
   anywhere else propagates immediately to the nearest of those three.
2. **The stalls are not independent.** E1 and E2 share a cause. E3 and E4
   share a path. E9 and E10 both sit under macOS scheduling pressure. Size
   the cushions for correlation until a measurement proves otherwise.

---

## 5. The cushion budget

```
required cushion = deterministic excursion + k * sigma_jitter + worst-case stall
                   (block geometry)          (routine variance)  (tail events)
```

The first term is exact. The second is measurable. The third is
heavy-tailed and can be bounded only by choosing an acceptable exceedance
probability.

### 5.1 B11 — the shm block boundary, derived

A producer deposits `P` frames every `P/f_s` seconds. A consumer removes `N`
frames every `N/f_s` seconds. Rates are equal, so there is no drift term.
The peak-to-trough occupancy excursion is

```
E(N,P) = N + P - gcd(N,P)
```

Over one repeat period, `lcm(N,P)/f_s` seconds, the reachable set of
"deposited minus removed" values is by Bezout's identity exactly the
multiples of `gcd(N,P)` in the range spanned. The consumer cannot remove
before its `N` frames exist, so the trough is 0 and the peak is the largest
reachable value below `N + P`.

If `N` and `P` are both powers of two, one divides the other, so
`gcd(N,P) = min(N,P)` and `E = max(N,P)`. **The derivation confirms the
deployed `max(N,P)` rule for power-of-two blocks, and shows it is wrong in
general.**

| `N` | `P` | `gcd` | `E` | `max(N,P)` | Agreement |
|-----|-----|-------|-----|------------|-----------|
| 32 | 64 | 32 | 64 | 64 | yes |
| 1024 | 64 | 64 | 1024 | 1024 | yes |
| **480** | 64 | 32 | **512** | 480 | **short by 32** |
| **1000** | 64 | 8 | **1056** | 1000 | **short by 56** |

**The `N < P` case.** At `N` = 32 and `P` = 64 the HAL wakes twice per
deposit, and one of those wakes finds nothing new. The clearance must be
`P`. **A smaller HAL block buys no cushion reduction at this boundary**;
below `N = P` the boundary latency is pinned at `P`.

**The advertised latency omits this term.** The published expression

```
one-way = 1 + (N_pi + L + 3)*P + G/2 + T_wire = 1106 frames at the reference
```

has a term for every JACK cycle stage, the slip ring, the wire, and `L`. It
has **no** term for `E(N,P)`.

| `N` | `E` unreported | Advertised | True | Understatement |
|-----|----------------|------------|------|----------------|
| 64 | 64 fr | 23.0 ms | 24.4 ms | 1.33 ms |
| 256 | 256 fr | 23.0 ms | 28.4 ms | 5.33 ms |
| 1024 | 1024 fr | 23.0 ms | **44.4 ms** | **21.33 ms** |

At `N` = 1024 the omitted term exceeds `G/2` and exceeds `L x P`. A DAW that
trusts the figure mis-aligns a recorded track by 42.7 ms round trip, with no
symptom.

### 5.2 B11 — the `J` cushion

`J` must cover the worst-case *relative lateness* of the two threads, plus
any excess of `maxBurst` over `N`.

**`maxBurst` is the largest IO block CoreAudio delivered in the window**
(`SA_Device.cpp:1745`), not a stall depth. Two values are on record, 472 and
272 frames, in two different sessions. Neither has been reproduced.

**Measured 2026-09-02 at N=64, P=64, J=128:** `maxBurst=64` across 40
consecutive windows, with `starveBlocks=0` and `lead=192` pinned. CoreAudio
did not bunch, so `J` was 2× the requirement. **At this `N` the boundary is
not under-cushioned.** Reproduce the bunching before buying cushion for it.

The ring is not the obstacle. At `N` = 1024, `2E + J` = 2560 against 4096
available, so about **1900 frames are free**. `J` = 128 uses 7 % of the
available cushion at one of the two documented failure sites.

### 5.3 B8 — the network loop budget

```
Budget:  L * P / f_s = 5.333 ms
Measured RTT (Mac -> pi): min 1.121  avg 2.386  max 7.517  sd 1.335 ms
Required L >= ceil(7.517 / 1.333) = 6 cycles.  Deployed L = 4.
```

`L` = 4 covers the mean plus 2 sigma and nothing more, against a
distribution with a 7.5 ms tail on a 2.4 ms mean.

The coupling constraint is `G/2 > L * P`:

| `L` | `G` | `L*P` | `G/2` | Holds? | Loop budget |
|-----|-----|-------|-------|--------|-------------|
| 4 | 512 | 256 | 256 | **NO, equal** | 5.33 ms |
| **6** | **512** | 384 | 256 | **NO** | 8.00 ms — **pi xruns ~2/s** |
| 4 | 1024 | 256 | 512 | yes | 5.33 ms |
| **6** | **1024** | 384 | 512 | **yes** | **8.00 ms** |

**The arithmetic prescribes `L` = 6 with `G` = 1024.** The documented failure
of `-l 6 -g 512` is a failure of the *pair*, not of `L` = 6. The
`L` = 6 / `G` = 1024 pair is untested.

### 5.4 B4 — the slip ring against jitter

```
Measured inter-arrival: p99 = 2.7 ms, max = 13.0 ms
A 13.0 ms gap drains 624 frames. Available from the midpoint: 512. Short by 112.
```

The full `G` is 21.33 ms, which exceeds 13 ms. **But the ring never operates
at its extreme**: the controller pins it at the midpoint by design, so the
usable headroom is `G/2`, not `G`. This distinction is the most common
sizing error in this class of system.

Drift over a 10 s settling window is 19 frames, against a 624-frame jitter
requirement. **Jitter sizes this ring by a factor of 33 over drift.** Drift
decides that a controller is needed. Jitter decides how big the ring is.

### 5.5 The complete budget

| Boundary | Deployed | Required | Verdict |
|----------|----------|----------|---------|
| B2, B9, B10, B12 | 0 | 0 | **correct** |
| B1 pi ALSA | 1.33 ms | >= 1 period | adequate, tight |
| B4 `G/2` | 512 fr | ~1028 fr | **UNDER by ~516** |
| B8 `L x P` | 256 fr | 384 fr | **UNDER by 128** |
| B11 `E` | `max(N,P)` | `N+P-gcd(N,P)` | **UNDER up to 56 for odd `N`** |
| B11 `J` | 128 fr | see 5.2 | **adequate at N=64; unmeasured under bunching** |
| shm ring | 4096 fr | 2560 fr worst | **OVER by ~1500 — free** |
| socket buffers, wire | kernel default | ~1 KB/cycle | **OVER ~100x — not the constraint** |

> **Every stock that costs latency is under-cushioned. Every stock that costs
> memory only is over-provisioned by one to two orders of magnitude.** The
> design has paid where payment is free, and economised where economising is
> what produces the artifacts.

Full compliance costs about 21 ms per direction. That is a real trade, not a
free win. Buy in the order of section 7.

---

## 6. Inevitable, and chosen

### 6.1 Structurally inevitable

1. **Cross-clock reconstruction error.** No VCXO exists on the pi. Every
   sample on the far side of B4 is an interpolated estimate. Bit-exactness is
   impossible by construction. Stop measuring for it.
2. **Eventual failure of any uncontrolled cross-clock buffer.** A bigger
   buffer buys time, never freedom.
3. **Tail exceedance at B8 and B11.** Scheduling delay on a general-purpose
   kernel has no finite maximum. **Publish a target exceedance rate. Do not
   target zero** — a design that targets zero has no stopping rule.
4. **Non-real-time NIC service on the pi.** Threading NAPI changes the
   distribution. It does not create a bound.
5. **A device-liveness transition costs a DAW re-select.** Host behaviour,
   outside the system. **Therefore never bounce the stack as a repair.**

### 6.2 A choice, not physics

1. **The B11 cushion.** The excursion is deterministic and about 1900 frames
   are free. **Any xrun at B11 is a choice.**
2. **The unreported `E(N,P)`.** Purely arithmetic.
3. **The slip-ring reset.** A ring 112 frames short does not require
   destroying the resampler state. Re-centring costs one interpolation
   error; a reset costs a discontinuity plus the PI settling time.
4. **Adaptive ring growth.** Every doubling is audible. Pin `G` correctly at
   the start and the learning phase disappears.
5. **One PI controller for two rings.** The measured disturbance is
   one-sided, and the ring that most needs control has none.
6. **Symmetric cushions on an asymmetric path.** 7.517 ms one way against
   2.871 ms the other.
7. **The host's `N` default.** A block size the system did not choose sets
   the largest single cushion in the path.
8. **Zeros as concealment.** A step to zero has energy at every frequency.
   It is the most audible possible failure, and the cheapest to implement.

---

## 7. The failure-propagation policy

Derived from three facts: latency **adds** across boundaries; independent
jitter **adds in quadrature**; a position correction is a discontinuity and
a rate correction is not.

- **P1 — Allocate cushion by marginal tail reduction, not evenly.** For a
  fixed latency budget the optimum equalises `|dp/dc|` across boundaries. For
  heavy tails that puts almost the whole budget at the single worst boundary.
  Cushion at a quiet boundary buys nothing and costs everything. **Zero
  cushion at B9, B10 and B12** — they are phase-locked, so `dp/dc` is exactly
  0 there.
- **P2 — Correct the rate before the position.** A few ppm of ratio nudge is
  inaudible and can run continuously. A cursor snap or a ring reset is a
  broadband discontinuity. **Every design that snaps routinely has mis-set
  its rate loop.**
- **P3 — Absorb locally; never propagate a stall as a reset.** A reset
  converts a local, bounded, one-cycle deficit into a global discontinuity
  plus a settling transient. It is almost always more expensive than the
  fault it answers.
- **P4 — Never let a downstream fault change an upstream geometry.** Ring
  growth, buffer renegotiation and timeline re-anchoring are all audible, and
  each invalidates every measurement taken across it. Geometry changes belong
  at start-up and at explicit user request.
- **P5 — Require `k` consecutive misses before any structural response.** One
  missed deadline is a sample from a tail. `k` = 3 to 5 costs a few cycles
  and removes essentially all false positives.
- **P6 — Conceal with a ramp, not a step.** Repeat the last block with a
  short cross-fade, or fade to silence across the block. This does not fix
  the fault. It converts a click into a dropout, which is far less
  objectionable.
- **P7 — Make the cushion asymmetric in the direction of the measured
  jitter.** Independent `L` and `G` per direction buy the same protection for
  about 60 % of the latency.
- **P8 — Repair at the smallest scope that fixes the fault.**

  ```
  cost 0 -> re-centre a ring by a few frames      (inaudible)
  cost 1 -> re-sync the netJACK cycle offset      (one dropout)
  cost 2 -> restart the pi slave only             (one dropout, pi side)
  cost 3 -> restart the daemon                    (one dropout, Mac side)
  cost 4 -> bounce the whole stack                (the user re-selects the
                                                   device in the DAW)
  ```

  Repairing a cost-1 fault with a cost-4 action bills the user for a repair
  they did not ask for.
- **P9 — Instrument the stocks, not the failures.** Every counter today
  records a *transition*. That is why both xrun counters can read zero
  through continuous audible clicking. A stock's **occupancy over time** is
  the observable that separates "holding with no margin" from "holding
  comfortably", and a transition counter cannot make that distinction.
- **P10 — State the target as a probability, not as zero.** For example,
  "fewer than one concealment per hour of playing" gives every cushion in
  section 5 an arithmetic criterion, and lets the work stop.

---

## 8. Open deviations

Where the running system departs from this model. Update this list when one
closes.

| Policy / finding | Deviation | Cost to close |
|------------------|-----------|---------------|
| P9 | The daemon cursors are not published into shm, and there is no HAL-side starvation counter. Ring occupancy is unobservable, so `E(N,P)` cannot be measured. | protocol bump |
| P6 | Concealment is a step: the HAL repeats the stale ring, and the daemon's recv slots read back as zeros. | small |
| §5.1 | `E(N,P)` is absent from the advertised latency. | arithmetic only |
| §5.1 | The clearance uses `max(N,P)`, exact only for power-of-two `N`. | small, latent |
| P5 | The re-anchor requires 3 consecutive windows. The snap fires on `k` = 1. | small |
| P1, §5.2 | `L` and `G` are unmeasured against the tails. `J` is measured adequate at N=64 (2026-09-02) and unmeasured under CoreAudio bunching. | a measurement campaign |
| P2 | The Mac side has only position corrections. Rule 3 forbids a rate correction there, so this deviation is accepted by design. | accepted |
| P3, P4, 6.2.3, 6.2.4 | The netadapter resets both rings and grows `G` on failure. | jack2 fork |
| 6.2.5 | One PI controller serves two rings, and the uncontrolled one faces the larger measured jitter. | jack2 fork |
| P7, 6.2.6 | `L` and `G` are symmetric across an asymmetric path. | jack2 fork |
| P10 | No exceedance target is stated anywhere. | a decision |

---

## 9. Reading this with the other documents

| Question | File |
|----------|------|
| What *should* a cushion be? | this file |
| What are the numbers right now? | `docs/DEBUGGING.md` |
| Where does the latency go? | `docs/LATENCY-MODEL.md` |
| Why is there a resampler at all? | `docs/CLOCK_WARS.md` |
| Where does the variance enter? | `docs/JITTER.md` |
| What will surprise me in the code? | `docs/idiosyncrasies.md` |
