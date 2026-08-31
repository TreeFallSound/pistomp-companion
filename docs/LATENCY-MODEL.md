# Latency model

End-to-end model of where time goes between the pi's IQaudIO codec and
the Mac's CoreAudio device, with every tunable named and its file/key
located. Pair with `JITTER.md` (where variance enters) and
`CLOCK_WARS.md` (why we have a resampler at all).

The chain is asymmetric: the Mac side is a single clock domain (clock
B), the Pi side has its own clock (clock A), and the cross-clock bridge
lives entirely in the Pi's `netadapter`. So tuning `netadapter` tunes
*the entire* cross-clock conversion.

---

## The picture

The chain is topologically lopsided: the IQaudIO codec (ADC + DAC) lives
entirely on the pi, and the Mac side terminates at the DAW — no DAC in the
model at all. But the two *directions* carry the same stages in reverse
order, one codec pass each, so their latency is equal. We draw both
explicitly anyway, because the resampler sits at opposite ends of the two
pictures.

### Recording (pi mic → Mac DAW)

```
  ┌──── pi (clock A) ───────────────────────┐         ┌──── Mac (clock B) ────────────────────────┐
  │                                         │   UDP   │                                           │
  │ [ADC] ──► ALSA ──► jackd ──► netadapter ─────────► netmanager ──► jackd ──► daemon ──► HAL ──► [DAW]
  │  T_adc    T_alsa    T_pj       T_g/T_l  │ T_wire  │   T_nm         T_mj       T_d     T_jf    │
  │                                         │         │                                           │
  │                       ▲                           ▲                                           │
  │              resampler lives here      no SRC: pure packet I/O                                │
  │              (only SRC in path)                                                               │
  └─────────────────────────────────────────┘         └───────────────────────────────────────────┘
```

### Playback (Mac DAW → pi speakers)

```
  ┌──── Mac (clock B) ─────────────────────────┐         ┌──── pi (clock A) ─────────────────────┐
  │                                            │   UDP   │                                       │
  │ [DAW] ──► HAL ──► daemon ──► jackd ──► netmanager ──► netadapter ──► jackd ──► ALSA ──► [DAC]
  │           T_jf      T_d       T_mj      T_nm│ T_wire │   T_g/T_l       T_pj    T_alsa  T_dac │
  │                                            │         │                                       │
  │                                                      ▲                                       │
  │                no SRC on Mac side             resampler lives here                            │
  └────────────────────────────────────────────┘         └───────────────────────────────────────┘
```

In both directions the **same single resampler** on the pi-side
netadapter bridges clock A ↔ the network-cycle clock (driven by Mac
clock B). Drift between A and B is absorbed by the netadapter slip ring
(G frames). Variance accumulates left-to-right in each direction — see
`JITTER.md` for the per-hop variance accounting.

---

## Latency contributions

The monitoring path's timing is runtime-discovered from the Pi at 44.1, 48,
or 96 kHz. Mac JACK uses the same sample rate and period, so one symbol `P`
covers both sides. `JitterFrames` is config-driven (`config.plist`, default
J=128 since 2026-08-30) and is a real per-direction cost on top of the
advertised figure — see the T_jf row. Every other term below is either a
multiple of `P`, a multiple of the netadapter ring `G`, or a fixed constant —
which is why the advertised figure is computed at runtime rather than baked in.

| Symbol | Stage | What it is | Frames | Frames @ P=64 | ms @ 48 k |
|--------|-------|------------|--------|---------------|-----------|
| T_adc  | Codec ADC          | Fixed group delay through the IQaudIO ADC | 1 | 1 | ~0.02 |
| T_alsa | ALSA capture       | `period_size × nperiods` on the pi ALSA backend (`-p × -n`) | N_pi · P | 128 | 2.67 |
| T_pj   | Pi jackd cycle     | One JACK period on the pi | P | 64 | 1.33 |
| T_g    | netadapter ring    | Slip-ring steady-state fill, `G/2` (see the note below) | G/2 | 512 | 10.67 |
| T_l    | netadapter cycles  | Network latency in cycles (`-l N` → N · P). **Deployed default = 4 cycles, max 30** (verified on-device via `Network latency : N cycles` log). | L · P | 256 | 5.33 |
| T_wire | UDP transit        | LAN one-way, direct cable. Dominated by NIC + switch fabric; ~0.35 ms on a direct cable, ~0.5–1 ms through one consumer switch (the model assumes direct cable). | 354 µs · f_s | 17 | ~0.35 |
| T_nm   | Mac netmanager     | One netjack cycle on the master side (≈ P_mac) | P | 64 | 1.33 |
| T_mj   | Mac jackd cycle    | One JACK period on the Mac (equal to P_pi) | P | 64 | 1.33 |
| T_jf   | HAL safety lead    | `JitterFrames`, config-driven (default J=128), returned as `kAudioDevicePropertySafetyOffset`. A real delay, not headroom: the daemon writes/reads one block ± this cushion clear of the HAL's head. | J | 128 | 2.67 |
| T_d    | Daemon shm publish | memcpy + atomic release — nanoseconds, ignore | 0 | 0 | 0 |
| T_dac  | Codec DAC          | Fixed group delay through the IQaudIO DAC | 1 | 1 | ~0.02 |

### The one-way leg

The two directions are symmetric: each carries exactly one codec pass, one
netadapter ring, one wire hop, and one JACK period at each of the four cycle
stages. So a single expression covers both:

```
one-way = 1 + (N_pi + L + 3)·P + G/2 + T_wire
        = 1 + 9·P + 512 + T_wire        (with N_pi = 2, L = 4, G = 1024)
```

At the deployed reference config (P = 64, f_s = 48 kHz) that is
`1 + 576 + 512 + 17 = 1106 frames`. That number is the *reference value*, not
a constant — at P = 128 the same expression gives 1682 frames, and at
P = 128 / 96 kHz it gives 1699.

This is implemented once, in `jb_one_way_latency_frames()`
(`jackbridge/shared/JackBridge.h`), and used by both the daemon's startup log
and the HAL. The daemon publishes its discovered period and sample rate into
shm (`JB_OFF_JACK_PERIOD_FRAMES`, `JB_OFF_JACK_SAMPLE_RATE`); the HAL reads
them in `_HW_Open` and again in `_HW_StartIO`, recomputes, and fires a
`kAudioDevicePropertyLatency` change notification if the value moved
(`SA_Device::_UpdateAdvertisedLatency`). If the daemon has not published yet,
the HAL advertises the reference config and corrects itself at StartIO.

### How it reaches the DAW

The HAL splits the advertised latency across two CoreAudio properties:

- `kAudioDevicePropertyLatency` = the one-way leg, reported for **both** input
  and output scope (CoreAudio's per-scope semantics). Each scope is genuinely
  one-way, so the DAW summing them gives the correct round trip.
- `kAudioDevicePropertySafetyOffset` = `JitterFrames` (config-driven, live
  J=128; the HAL adopts the daemon's published value in
  `_RefreshSafetyOffset`, `SA_Device.cpp:1965`). It is a real per-direction
  delay — the daemon holds each cursor one block ± this cushion clear of
  the HAL's head — and the DAW adds it on top of the one-way figure, so
  the model excludes it from `jb_one_way_latency_frames()` to avoid
  double-counting. Note it does not function as a producer-side write
  lead the daemon reacts to: the daemon owns J, the HAL only reports it
  (raising SafetyOffset in the DAW does nothing; raising `JitterFrames`
  in `config.plist` does — see `docs/investigation-bug1.md` and
  `docs/JITTER.md`'s SafetyOffset experiment for the history).

The DAW sums Latency(in) + Latency(out) + SafetyOffset×2 + its own buffers.

### The monitoring trip

```
Σ = 2 × one-way
```

Σ is what a guitarist monitoring through the Mac hears: a signal that enters
the pi's ADC, traverses the whole chain to the Mac, and comes back out the
pi's DAC. At the deployed reference config that is **2212 frames / 46.1 ms @
48 kHz** before the separately reported SafetyOffset.
There is no de-duplication to apply — the forward leg carries T_adc and the
return leg carries T_dac, one codec pass each, and every other stage is
genuinely traversed twice.

| Measurement scenario | What to expect |
|----------------------|----------------|
| **Monitoring** (pi ADC → Mac → pi DAC)  | Σ = 2 × one-way = **2212 frames / 46.1 ms** at reference |
| **One-way recording** (pi ADC → Mac DAW, no return) | one-way = **1106 frames / 23.0 ms** at reference |
| **One-way playback** (Mac DAW → pi DAC) | one-way, same figure (the legs are symmetric) |
| **Pure-digital round-trip** (Mac plays → returns via JackBridge loopback, no codec) | Σ − T_adc − T_dac ≈ **2210 frames** |
| **Hardware loopback round-trip** (pi DAC cabled into pi ADC) | ≈ 2 × Σ — the monitoring trip, twice |

The DAW's own internal buffer (typically 128–512 frames) sits on top of all of
these.

### Notes on the math

- **T_g uses G/2, not G.** The slip ring's controller resamples to keep
  fill near the midpoint; you only see the full G as headroom for
  bursts, not as steady-state latency. With `-g 512` that's 5.3 ms
  steady-state but 10.6 ms of burst tolerance.
- **T_alsa is the dominant pi-side audio buffer.** jackd's `-n 2` means
  ALSA holds 2 periods worth of frames before jackd sees them. The
  comment in `pistomp-arch/files/jackdrc:19` shows `-n 2` hardcoded.
- **T_l is not the same as T_wire.** `-l` is the number of *netjack
  cycles* of cushion the netadapter requests against network jitter,
  expressed in period-frames; T_wire is the actual UDP transit time on
  the wire. Both add up.
- **Σ is already a round trip.** Σ = 2 × the one-way leg. Don't double it
  again for "round-trip monitoring" — that scenario *is* Σ. Only a hardware
  loopback (pi DAC cabled back into pi ADC) traverses the chain twice.
- **The advertised figure is not a build constant.** Five of the eight terms
  scale with P, so the HAL computes it from the Pi's discovered timing at
  device open and at StartIO. See `jb_one_way_latency_frames()`.

---

## What the model does not cover: the mod-host graph

Verified on the live pi, 2026-08-30. Full investigation: `docs/mod-latency.md`.

**The model covers the transport path only.** Every term above describes the
path from the pi codec to the Mac HAL. None of them describes `mod-host`. The
plugin graph adds delay, and no term in `jb_one_way_latency_frames()` accounts
for it.

**The four inputs are not homogeneous.** This is why the omission matters.

| Mac input | JACK port | Signal | Passes mod-host |
|-----------|-----------|--------|-----------------|
| In1 / In2 | `netadapter:playback_1` / `_2` | dry | no |
| In3 / In4 | `netadapter:playback_3` / `_4` (ModOut1 / ModOut2) | wet | yes |

One advertised figure describes both pairs. It is correct for the dry pair and
too small for the wet pair.

**JACK reports the wet pair as faster than the dry pair.** Read with
`JACK_PROMISCUOUS_SERVER=jack jack_lsp -l` on the pi — jackd runs as the user
`jack`, so a plain `jack_lsp` from an ssh login cannot reach the server:

    system:capture_1          capture latency = [ 64 64 ]   dry, hardware ADC
    netadapter:playback_1     capture latency = [ 64 64 ]   dry pair
    netadapter:playback_3     capture latency = [  0  0 ]   wet pair

The wet path is the dry path plus the plugin graph, so its latency cannot be
lower. `mod-host` implements no JACK latency callback, so nothing propagates
through it and the ports report zero.

**The number is not obtainable today.** `/usr/bin/mod-host` contains no
occurrence of the string `latency`, in any case. It has no protocol command for
it. mod-ui 0.99.8 serves no latency route. Only 19 of the 526 LV2 bundles on
the pi declare `lv2:reportsLatency`, so summing the plugins gives a lower bound
and not an answer.

**Per-stream advertisement would not reach the DAW either.** CoreAudio has
`kAudioStreamPropertyLatency` per stream, beside `kAudioDevicePropertyLatency`
per device, and the two input pairs are two separate streams. The driver
returns a hardcoded 0 for the stream property today
(`jackbridge/driver/JackBridge/Plug-In/SA_Device.cpp:1310`). Implementing it
does not help: JUCE reads stream 0 only, and Ardour collects the per-stream
values and then discards them. Both claims come from upstream source; see
`docs/mod-latency.md` for the citations.

**What to do about it.** Treat the advertised figure as the dry-pair number.
Do not trust it for In3 / In4.

The audible consequence is a phase error, not a delay a user can hear on its
own. A user who mixes In1 / In2 against In3 / In4 in the DAW is summing two
copies of one signal at different delays, and the DAW aligns both by the same
advertised figure. Delay compensation therefore corrects neither pair
relatively. Comb filtering is the result.

---

## Tunables — what to change and where

Ordered roughly by latency impact (biggest first), with the latency
delta you get per unit of change.

| Symbol | Knob | Where | Default | Impact on latency (frames per unit) |
|--------|------|-------|---------|-------------------------------------|
| G | netadapter ring size (`-g N`) | `jackbridge/pi/bin/jackbridge-pi-up:70` (deployed: `/usr/local/libexec/jackbridge/jackbridge-pi-up`) | `1024` (was adaptive) | **0.5** — half a frame steady-state per ring frame; full frame in burst headroom |
| P_pi | Pi JACK period (`-p N`) | Pi image JACK configuration | discovered live from the Pi | T_pj scales 1:1, T_alsa scales N_pi:1 — **the largest knob** |
| N_pi | ALSA periods (`-n N`) | `pistomp-arch/files/jackdrc:19` (hardcoded `-n 2`) | `2` | P_pi frames per period — biggest non-G one-shot saving if dropped to 1 (but risky) |
| L | netadapter network latency (`-l N`, cycles, range 0–30) | `jackbridge/pi/bin/jackbridge-pi-up:70` | `4` | P_pi frames per cycle |
| P_mac | Mac JACK period | coordinator runtime arguments | equal to discovered P_pi | Must equal P_pi or netJACK2 resampler chokes |
| J | HAL safety lead (`JitterFrames`) | `config.plist` `JitterFrames`, adopted by driver and daemon at runtime | `128` | Real per-direction delay (2×J round trip), reported via `kAudioDevicePropertySafetyOffset`; the daemon enforces it as the cushion in both ring projections |
| f_s | Sample rate | discovered live from the Pi | `44100`, `48000`, or `96000` | All times are `frames / f_s` |
| Q | netadapter resampler quality (`-q N`, **0 = lowest, 4 = highest**) | `jackbridge/pi/bin/jackbridge-pi-up:70` | `0` (we set it explicitly) | No latency impact — only CPU/fidelity |
| MTU | netJACK MTU | Pi runtime | `1500` | Affects T_wire only at jumbo-frame scale |
| RT prio | jackd realtime priority | Pi image / Mac launcher | `75` | No direct latency; affects jitter |
| Storm threshold | Auto-restart on xrun storm | `JACKBRIDGE_XRUN_THRESHOLD` env | `50/s` | Recovers from degraded state |

### Knobs that DON'T affect latency

`jackbridge/installer/config.plist` holds only persistent user settings —
runtime timing is discovered, not configured — so none of its keys move
latency:

- `ClockDeviceUID` — picks *which* clock B is, not how the buffers are sized.
- `NetworkInterface` — routing, not buffering.
- `PiHostname` — reachability, MOD-UI, SSH, and probes only.

Auto-wiring topology (`jackbridge-pi-up`'s `jack_connect` pass) and log
verbosity likewise change no buffer size.

---

## How the knobs interact

A few non-obvious couplings:

### P_pi and P_mac must match

`CLOCK_WARS.md` explains why: netJACK2's master/slave handshake expects
equal cycle sizes. If they
differ, netadapter's resampler throws `WriteResample error` on cycle 1
and thrashes. This is enforced in practice rather than configured — the
coordinator probes the Pi and passes the same `--rate` / `--period` to
`jackbridge/installer/jackd-launch`, which hard-fails if no period arrives.

### G interacts with measured network jitter

`-g 512` gives 10.6 ms of burst tolerance. JITTER.md §5 measured network
inter-arrival p99=2.7 ms, max=13 ms. So an extreme burst can still
overflow this ring. With the auto-restart sentinel in place, that
becomes "3 s of audio dropout, then recovery" rather than "permanently
degraded ring size for the rest of the session." Pick G to suit your
tolerance:

| G (frames) | Steady (ms) | Burst tolerance (ms) | Trade |
|------------|-------------|----------------------|-------|
| 256        | 2.67        | 5.33                 | Below network max; only viable at L=1 with stable RTT. |
| 512        | 5.33        | 10.6                 | Resampler unstable under mod-host load (tested 2026-08-30). Do not use with plugins. |
| 1024       | 10.6        | 21.3                 | **Deployed** (`/etc/default/jackbridge JACKBRIDGE_NET_RING=1024`). Stable floor with mod-host. |
| 2048       | 21.3        | 42.6                 | "Set and forget." |
| 4096       | 42.6        | 85.3                 | Studio session, no storm risk acceptable. |

### J (JitterFrames) — now load-bearing

J = **128 frames** as of 2026-08-30 (`JB_JITTER_FRAMES`, lowered from 320 the
same day). It is user-set in `config.plist`, so read the live value from
`just shm` rather than assuming the default.

The daemon seeds the send cursor at `halInputReadHead + block + J` and the recv
cursor at `halOutputWriteHead − block − J`. Both advance by one JACK period;
the daemon snaps either cursor only when its shared projector window reports a
hard hazard.

`block` is `max(HAL buffer size, JACK period)`, and it is separate from J on
purpose. Both heads are published *before* the HAL copies that cycle's block,
so a head of H means the HAL is about to touch `[H, H+block)`. The block term
is the mandatory clearance; J is cushion on top of it. J therefore means
cushion and nothing else.

J is reported via `kAudioDevicePropertySafetyOffset`. The DAW adds it on
top of the one-way latency from `kAudioDevicePropertyLatency`, so it must
NOT appear in `jb_one_way_latency_frames()`.

**Sizing constraint:** J must cover `maxBurst` in the worst case where JACK
and HAL stall together. A 2-hour live session observed `maxBurst=472`
(7.4×P). The correlation between `daemonXruns` and `maxBurst` in that
session was inconclusive (G=512 was causing independent JACK xruns). A
stable session with `daemonXruns=0` in quiet windows is needed to measure
true correlation. If stalls are uncorrelated, J can shrink to
`maxBurst − stall_cycles×P`; if correlated, J must equal `maxBurst`.
At J=320 (5×P) the 472-frame burst produces a 152-frame starvation in the
fully-correlated case. At the current J=128 (2×P) the same burst produces a
344-frame starvation, so lowering J widened this risk rather than closing it.
**Open risk — needs measurement.**

**J=0 is the tightest correct alignment.** The upstream read position is a
free-running cursor whose target is `halOutputWriteHead − block − J`. The HAL
has written up to the write head, exclusive, so the newest complete block is
`[W−block, W)`. The block term is mandatory clearance; J is cushion on top.

The same geometry applies to the send cursor. The daemon requires:

    ring_frames > 2 × max(HAL block, JACK period) + J

This is a strict safety condition, not a tuning clamp. With the 4096-frame ring,
J=128 requires `max(N, P) < 1984`; J=0 permits values below 2048. The daemon
raises a fault and logs the geometry when a host requests an invalid size.

J is reported via `kAudioDevicePropertySafetyOffset`. The DAW adds it on top of
the one-way latency from `kAudioDevicePropertyLatency`, so it must NOT appear in
`jb_one_way_latency_frames()`.

### Q is free latency-wise but not CPU-wise

`-q` only changes resampler quality (filter taps). Lower Q = cheaper Pi
CPU; we've set `-q 0` because (a) we can't be bit-exact anyway so
fidelity past "inaudible" is wasted (`CLOCK_WARS.md`) and (b) freeing
Pi CPU helps the netadapter cycle hit its budget under mod-host load.

---

## Quick recipes

All Σ figures are **monitoring trip** (pi ADC → Mac → pi DAC) = 2 × one-way.

**Current runtime behavior (deployed 2026-08-30, P=64, f_s=48000):**
- L=4 (`JACKBRIDGE_NET_LATENCY=4`), G=1024 (`JACKBRIDGE_NET_RING=1024`), J=128 (`JitterFrames` in `~/Library/Application Support/JackBridge/config.plist`, lowered from 320 on 2026-08-30)
- One-way latency advertised by HAL = `jb_one_way_latency_frames()` = 1106 frames = 23.0 ms
- SafetyOffset = J = 128 frames = 2.67 ms
- DAW total one-way = 1106 + 128 = **1234 frames = 25.7 ms**
- Monitoring trip (Σ) = 2 × 1234 = **2468 frames = 51.4 ms**
- Pi sample rate and period are discovered at startup; Mac JACK receives the same values.
- The advertised latency follows the discovered timing automatically.

Tune the Pi JACK period on the Pi itself. The next Mac startup probe follows
that value; the settings editor does not expose a period control.

**Read the figure your stack actually computed:**

```sh
just logs   # daemon: "latency model: period=… f_s=… -> one-way=… monitoring trip=…"
            # driver: "health … leadJitter=… daemonXruns=…"
```

Reference values from the model (L=4, G=1024, J=128 — the live default),
for orientation:

| P | f_s | one-way (excl J) | + J=128 | monitoring trip |
|---|-----|-----------------|---------|-----------------|
| 64 | 48000 | 1106 | 1234 | **2468 (51.4 ms)** |
| 128 | 48000 | 1682 | 1810 | 3620 (75.4 ms) |
| 64 | 44100 | 1105 | 1233 | 2466 (55.9 ms) |

**Diagnose where latency lives in YOUR setup:**
- Send a known transient (handclap, click track) from DAW to pi headphones, record back.
- The recorded delay is the monitoring trip ≈ Σ + your DAW's monitoring path latency.
- Bisect by changing one knob at a time and re-measuring.

---

## Where these numbers come from

- T_alsa, T_pj, T_mj, T_nm are from the JACK / ALSA buffer math (frames ÷ sample rate).
- T_g midpoint behavior is documented in jack2's `JackAudioAdapter::PushAndPull` — the controller targets midpoint via the resampler ratio.
- T_jf (JitterFrames) defaults to J=128 (`JB_JITTER_FRAMES`, changed from 320 on 2026-08-30), enforced as a write lead ahead of the send cursor's target and a read trail behind the recv cursor's target in `RingProjector.hpp`. It is user-set in `config.plist`, so read the live value from `just shm` rather than assuming the default. `J = 0` now means the tightest correct alignment and costs nothing beyond the mandatory block clearance; before the block term was separated out it cost a full ring (4096 frames, 85 ms) in each direction, invisible to both the advertised figure and SafetyOffset. Subtract accordingly when reading a measurement taken before that fix. See `docs/investigation-bug1.md` for the original bug and `jackbridge/daemon/RingProjector.hpp` for the implementation.
- The model itself lives in `jb_one_way_latency_frames()` in `jackbridge/shared/JackBridge.h`, alongside the constants it uses (`JB_ALSA_PERIODS_PI`, `JB_NET_LATENCY_CYCLES`, `JB_NETADAPTER_RING_FRAMES`, `JB_WIRE_TRANSIT_MICROS`, `JB_JITTER_FRAMES`). L and G no longer have to be kept in step by hand: the four-argument form takes the pair the daemon published from `config.plist` (`NetLatency`, `NetRing`), and the HAL uses that. `JB_NET_LATENCY_CYCLES` / `JB_NETADAPTER_RING_FRAMES` are the fallback for a region no daemon has attached to yet — keep them equal to the `config.plist` defaults. The other constants are still hand-maintained: change a Pi-side knob they describe and you must change them, or the advertised figure drifts from reality. `just shm` prints the published pair and the resulting one-way figure.
- Network latency default (`-l 2`, max 30) verified on the live pi by loading netadapter under a probe client name and reading the `Network latency : N cycles` line from `journalctl -u jack` (jack2 1.9.22 on Arch).
- T_adc / T_dac are codec group-delay values from the IQaudIO datasheet (low ms).
- ms numbers are computed for 48 kHz unless stated; scale for other rates.
- T_wire is an assumption, not a measurement: 354 µs, the direct-cable case. A consumer switch in the path adds ~0.15–0.65 ms that the model does not account for.
