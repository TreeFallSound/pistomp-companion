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
covers both sides. `JitterFrames` is fixed at 0. Every term below is either a
multiple of `P`, a multiple of the netadapter ring `G`, or a fixed constant —
which is why the advertised figure is computed at runtime rather than baked in.

| Symbol | Stage | What it is | Frames | Frames @ P=64 | ms @ 48 k |
|--------|-------|------------|--------|---------------|-----------|
| T_adc  | Codec ADC          | Fixed group delay through the IQaudIO ADC | 1 | 1 | ~0.02 |
| T_alsa | ALSA capture       | `period_size × nperiods` on the pi ALSA backend (`-p × -n`) | N_pi · P | 128 | 2.67 |
| T_pj   | Pi jackd cycle     | One JACK period on the pi | P | 64 | 1.33 |
| T_g    | netadapter ring    | Slip-ring steady-state fill, `G/2` (see the note below) | G/2 | 256 | 5.33 |
| T_l    | netadapter cycles  | Network latency in cycles (`-l N` → N · P). **jack2 1.9.22 default = 2 cycles, max 30** (verified on-device via `Network latency : N cycles` log). | L · P | 128 | 2.67 |
| T_wire | UDP transit        | LAN one-way, direct cable. Dominated by NIC + switch fabric; ~0.35 ms on a direct cable, ~0.5–1 ms through one consumer switch (the model assumes direct cable). | 354 µs · f_s | 17 | ~0.35 |
| T_nm   | Mac netmanager     | One netjack cycle on the master side (≈ P_mac) | P | 64 | 1.33 |
| T_mj   | Mac jackd cycle    | One JACK period on the Mac (equal to P_pi) | P | 64 | 1.33 |
| T_d    | Daemon shm publish | memcpy + atomic release — nanoseconds, ignore | 0 | 0 | 0 |
| T_jf   | HAL safety lead    | Fixed `JitterFrames=0`, returned as `kAudioDevicePropertySafetyOffset` | 0 | 0 | 0 |
| T_dac  | Codec DAC          | Fixed group delay through the IQaudIO DAC | 1 | 1 | ~0.02 |

### The one-way leg

The two directions are symmetric: each carries exactly one codec pass, one
netadapter ring, one wire hop, and one JACK period at each of the four cycle
stages. So a single expression covers both:

```
one-way = 1 + (N_pi + L + 3)·P + G/2 + T_wire
        = 1 + 7·P + 256 + T_wire        (with N_pi = 2, L = 2, G = 512)
```

At the reference config (P = 64, f_s = 48 kHz) that is
`1 + 448 + 256 + 17 = 722 frames`. That number is the *reference value*, not a
constant — at P = 128 the same expression gives 1170 frames, and at
P = 128 / 96 kHz it gives 1187.

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
- `kAudioDevicePropertySafetyOffset` = `JitterFrames` = **0**. It is reported
  for completeness and is inert at its fixed value. Note that it does *not*
  currently function as a producer-side write lead: the daemon never consumes
  it (`docs/investigation-bug1.md`, verified), and raising it to 192/1024/4096
  had no measurable effect on click rate or on the driver's `nearMiss` counter
  (`docs/JITTER.md` — "the SafetyOffset experiment"). Treat it as a reported
  constant, not as a working knob.

The DAW sums Latency(in) + Latency(out) + SafetyOffset×2 + its own buffers.

### The monitoring trip

```
Σ = 2 × one-way
```

Σ is what a guitarist monitoring through the Mac hears: a signal that enters
the pi's ADC, traverses the whole chain to the Mac, and comes back out the
pi's DAC. At the reference config that is **1444 frames / 30.1 ms @ 48 kHz**.
There is no de-duplication to apply — the forward leg carries T_adc and the
return leg carries T_dac, one codec pass each, and every other stage is
genuinely traversed twice.

| Measurement scenario | What to expect |
|----------------------|----------------|
| **Monitoring** (pi ADC → Mac → pi DAC)  | Σ = 2 × one-way = **1444 frames / 30.1 ms** at reference |
| **One-way recording** (pi ADC → Mac DAW, no return) | one-way = **722 frames / 15.0 ms** at reference |
| **One-way playback** (Mac DAW → pi DAC) | one-way, same figure (the legs are symmetric) |
| **Pure-digital round-trip** (Mac plays → returns via JackBridge loopback, no codec) | Σ − T_adc − T_dac ≈ **1442 frames** |
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

## Tunables — what to change and where

Ordered roughly by latency impact (biggest first), with the latency
delta you get per unit of change.

| Symbol | Knob | Where | Default | Impact on latency (frames per unit) |
|--------|------|-------|---------|-------------------------------------|
| G | netadapter ring size (`-g N`) | `jackbridge/pi/bin/jackbridge-pi-up:70` (deployed: `/usr/local/libexec/jackbridge/jackbridge-pi-up`) | `512` (was adaptive) | **0.5** — half a frame steady-state per ring frame; full frame in burst headroom |
| P_pi | Pi JACK period (`-p N`) | Pi image JACK configuration | discovered live from the Pi | T_pj scales 1:1, T_alsa scales N_pi:1 — **the largest knob** |
| N_pi | ALSA periods (`-n N`) | `pistomp-arch/files/jackdrc:19` (hardcoded `-n 2`) | `2` | P_pi frames per period — biggest non-G one-shot saving if dropped to 1 (but risky) |
| L | netadapter network latency (`-l N`, cycles, range 0–30) | `jackbridge/pi/bin/jackbridge-pi-up:70` (currently unset → default) | `2` (jack2 1.9.22, verified on-device) | P_pi frames per cycle |
| P_mac | Mac JACK period | coordinator runtime arguments | equal to discovered P_pi | Must equal P_pi or netJACK2 resampler chokes |
| J | HAL safety lead (`JitterFrames`) | fixed runtime default in driver and daemon | `0` | Reported via `kAudioDevicePropertySafetyOffset`, but not consumed as a write lead — see "J is reported, not enforced" below |
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
| 256        | 2.67        | 5.33                 | Tighter, but well under measured network max (13 ms). |
| 512        | 5.33        | 10.6                 | **Current default** (`jackbridge-pi-up:70`, `jack_load netadapter -i "… -g 512"`). ~4× headroom over measured p99 (2.7 ms); storm-restart is the safety net for the gap to measured max. |
| 1024       | 10.6        | 21.3                 | Comfortably above measured max jitter. |
| 2048       | 21.3        | 42.6                 | "Set and forget." |
| 4096       | 42.6        | 85.3                 | Studio session, no storm risk acceptable. |

### J (JitterFrames) is reported, not enforced

J is fixed at 0 and is currently **cosmetic**. Two findings pin this down:

- `docs/investigation-bug1.md` (verified): the daemon and the HAL use two
  independent modulo counters on the same ring. Nothing consumes J as a write
  lead, so raising it would not actually move the daemon's write point.
- `docs/JITTER.md` ("the SafetyOffset experiment"): tested at 192, 1024, and
  4096, click rate was identical and the driver's `nearMiss` counter stayed at
  0. The bottleneck is Mac jackd's netJACK2 master thread under preemption,
  not HAL scheduling.

So J is reported through `kAudioDevicePropertySafetyOffset` and enters the
DAW's round-trip sum, but it buys no jitter absorption today. If it is ever
made load-bearing, the design constraint is this: because the Mac is one clock
end-to-end (CLOCK_WARS.md "where the SRC actually lives"), J needs no
controller — it would be a constant lead, sized to absorb thread scheduling
jitter on the Mac (`docs/idiosyncrasies.md` — daemon's JACK thread vs HAL's IO
proc), not clock drift. If you ever broke the same-clock assumption
(e.g. ran jackd on a different physical device than `ClockDeviceUID`
selected), J would need to grow to slip-ring proportions or you'd need
to add SRC on the Mac — which `CLAUDE.md` explicitly forbids.

### Q is free latency-wise but not CPU-wise

`-q` only changes resampler quality (filter taps). Lower Q = cheaper Pi
CPU; we've set `-q 0` because (a) we can't be bit-exact anyway so
fidelity past "inaudible" is wasted (`CLOCK_WARS.md`) and (b) freeing
Pi CPU helps the netadapter cycle hit its budget under mod-host load.

---

## Quick recipes

All Σ figures are **monitoring trip** (pi ADC → Mac → pi DAC) = 2 × one-way.

**Current runtime behavior:**
- Pi sample rate and period are discovered at startup; Mac JACK receives the same values.
- Supported sample rates are 44100, 48000, and 96000 Hz.
- `JitterFrames=0`; no persistent timing keys are stored in the home plist.
- The advertised latency follows the discovered timing automatically.

Tune the Pi JACK period on the Pi itself. The next Mac startup probe follows
that value; the settings editor does not expose a period control.

**Read the figure your stack actually computed:**

```sh
just logs   # daemon: "latency model: period=… f_s=… -> one-way=… monitoring trip=…"
            # driver: "latency: period=… f_s=… -> N frames per scope"
```

Reference values from the model, for orientation:

| P | f_s | one-way | monitoring trip |
|---|-----|---------|-----------------|
| 64 | 48000 | 722 | 1444 (30.1 ms) |
| 128 | 48000 | 1170 | 2340 (48.8 ms) |
| 64 | 44100 | 721 | 1442 (32.7 ms) |
| 128 | 96000 | 1187 | 2374 (24.7 ms) |

**Diagnose where latency lives in YOUR setup:**
- Send a known transient (handclap, click track) from DAW to pi headphones, record back.
- The recorded delay is the monitoring trip ≈ Σ + your DAW's monitoring path latency.
- Bisect by changing one knob at a time and re-measuring.

---

## Where these numbers come from

- T_alsa, T_pj, T_mj, T_nm are from the JACK / ALSA buffer math (frames ÷ sample rate).
- T_g midpoint behavior is documented in jack2's `JackAudioAdapter::PushAndPull` — the controller targets midpoint via the resampler ratio.
- T_jf is fixed at 0 in both `jackbridge/daemon/JackBridge.cpp` (`kDefaultJitterFrames`) and `SA_Device.cpp`; the reasoning for leaving it there is in `docs/investigation-bug1.md` and `docs/JITTER.md`.
- The model itself lives in `jb_one_way_latency_frames()` in `jackbridge/shared/JackBridge.h`, alongside the constants it uses (`JB_ALSA_PERIODS_PI`, `JB_NET_LATENCY_CYCLES`, `JB_NETADAPTER_RING_FRAMES`, `JB_WIRE_TRANSIT_MICROS`). Change a Pi-side knob and you must change the matching constant there, or the advertised figure drifts from reality.
- Network latency default (`-l 2`, max 30) verified on the live pi by loading netadapter under a probe client name and reading the `Network latency : N cycles` line from `journalctl -u jack` (jack2 1.9.22 on Arch).
- T_adc / T_dac are codec group-delay values from the IQaudIO datasheet (low ms).
- ms numbers are computed for 48 kHz unless stated; scale for other rates.
- T_wire is an assumption, not a measurement: 354 µs, the direct-cable case. A consumer switch in the path adds ~0.15–0.65 ms that the model does not account for.
