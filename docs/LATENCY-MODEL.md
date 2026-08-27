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

JackBridge is bidirectional but the two directions are *not symmetric*:
the IQaudIO codec (ADC + DAC) lives entirely on the pi. The Mac side
terminates at the DAW — no DAC in the model at all. So we draw the two
directions explicitly.

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
or 96 kHz. Mac JACK uses the same sample rate and period. `JitterFrames` is
fixed at 0. Values below that depend on the deployed Pi configuration.

| Symbol | Stage | What it is | Frames | ms @ 48 k |
|--------|-------|------------|--------|-----------|
| T_adc  | Codec ADC          | Fixed group delay through the IQaudIO ADC | ~1 | ~0.02 |
| T_alsa | ALSA capture       | `period_size × nperiods` on the pi ALSA backend (`-p × -n`) | discovered | — |
| T_pj   | Pi jackd cycle     | One JACK period on the pi (`P_pi`) | discovered | — |
| T_l    | netadapter cycles  | Network latency in cycles (`-l N` → N · P_pi). **jack2 1.9.22 default = 2 cycles, max 30** (verified on-device via `Network latency : N cycles` log). | 128 | 2.67 |
| T_wire | UDP transit        | LAN one-way, direct cable. Dominated by NIC + switch fabric; sub-millisecond on a direct cable, ~0.5–1 ms through one consumer switch. | ~17 | ~0.35 |
| T_nm   | Mac netmanager     | One netjack cycle on the master side (≈ P_mac) | discovered | — |
| T_mj   | Mac jackd cycle    | One JACK period on the Mac (equal to P_pi) | discovered | — |
| T_d    | Daemon shm publish | memcpy + atomic release — nanoseconds, ignore | 0 | 0 |
| T_jf   | HAL safety lead    | Fixed `JitterFrames=0`, returned as `kAudioDevicePropertySafetyOffset` | 0 | 0 |
| T_dac  | Codec DAC          | Fixed group delay through the IQaudIO DAC | ~1 | ~0.02 |
| **Σ**  | **Monitoring trip** | Sum of fixed and discovered contributions | **discovered** | **discovered** |

The HAL splits the advertised total across two CoreAudio properties so
the host can act on each correctly (SA_Device.cpp):

- `kAudioDevicePropertyLatency = kBaseLatencyFrames = 722` — everything
  in the table except T_jf. Pure report; the DAW adds it to its
  round-trip estimate.
- `kAudioDevicePropertySafetyOffset = JitterFrames` — *also* enters the
  DAW's round-trip estimate, **and** tells CoreAudio to schedule the
  IOProc that many frames earlier in sampleTime. That earlier
  scheduling is what realises the safety lead in practice.

The DAW sums them, so the advertised round-trip is `722 + JitterFrames`
just as before — only now the split is honest and retuning JitterFrames
also retunes the actual scheduling, not just the reported number.

### What this sum is

The 915-frame total represents the **monitoring path**: a signal that
enters the pi's ADC, traverses the whole chain to the Mac, and comes
back out the pi's DAC. This is what a guitarist hears when monitoring
through the Mac.

| Measurement scenario | What to do to Σ |
|----------------------|-----------------|
| **Monitoring** (in pi ADC → Mac → out pi DAC)  | Σ as-is = **915 frames / 19.1 ms** |
| **One-way recording** (pi ADC → Mac DAW, no return) | Σ − T_dac (drop the playback codec leg from `T_jf` onward; recording stops at HAL/DAW) |
| **One-way playback** (Mac DAW → pi DAC) | Σ − T_adc (no ADC at start; DAW source is digital) |
| **Pure-digital round-trip** (Mac plays signal → returns via JackBridge loopback, no codec) | ≈ 2 × (Σ − T_adc − T_dac) — both digital chains, no codec passes |
| **Hardware loopback round-trip** (pi DAC output cabled into pi ADC input) | ≈ 2 × Σ — full monitoring trip, twice |

Σ represents one full traversal in/out of the codec, which is the
useful unit for most listener-facing reasoning. The DAW's own internal
buffer (typically 128–512 frames) sits on top of all of these.

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
- **Round-trip latency** ≈ 2 · Σ if recording and monitoring through
  the Mac, but in practice DAW monitoring/effects sit between, so the
  RTT depends on your signal flow.

---

## Tunables — what to change and where

Ordered roughly by latency impact (biggest first), with the latency
delta you get per unit of change.

| Symbol | Knob | Where | Default | Impact on latency (frames per unit) |
|--------|------|-------|---------|-------------------------------------|
| G | netadapter ring size (`-g N`) | `jackbridge/pi/bin/jackbridge-pi-up:20` (deployed: `/usr/local/libexec/jackbridge/jackbridge-pi-up`) | `512` (was adaptive) | **0.5** — half a frame steady-state per ring frame; full frame in burst headroom |
| P_pi | Pi JACK period (`-p N`) | Pi image JACK configuration | discovered live from the Pi | T_pj scales 1:1, T_alsa scales N_pi:1 — **the largest knob** |
| N_pi | ALSA periods (`-n N`) | `pistomp-arch/files/jackdrc:19` (hardcoded `-n 2`) | `2` | P_pi frames per period — biggest non-G one-shot saving if dropped to 1 (but risky) |
| L | netadapter network latency (`-l N`, cycles, range 0–30) | `jackbridge/pi/bin/jackbridge-pi-up:20` (currently unset → default) | `2` (jack2 1.9.22, verified on-device) | P_pi frames per cycle |
| P_mac | Mac JACK period | coordinator runtime arguments | equal to discovered P_pi | Must equal P_pi or netJACK2 resampler chokes |
| J | HAL safety lead (`JitterFrames`) | fixed runtime default in driver and daemon | `0` | 1:1 — surfaces as `kAudioDevicePropertySafetyOffset` |
| f_s | Sample rate | discovered live from the Pi | `44100`, `48000`, or `96000` | All times are `frames / f_s` |
| Q | netadapter resampler quality (`-q N`, **0 = lowest, 4 = highest**) | `jackbridge/pi/bin/jackbridge-pi-up:20` | `0` (we set it explicitly) | No latency impact — only CPU/fidelity |
| MTU | netJACK MTU | Pi runtime | `1500` | Affects T_wire only at jumbo-frame scale |
| RT prio | jackd realtime priority | Pi image / Mac launcher | `75` | No direct latency; affects jitter |
| Storm threshold | Auto-restart on xrun storm | `JACKBRIDGE_XRUN_THRESHOLD` env | `50/s` | Recovers from degraded state |

### Knobs that DON'T affect latency

- `ClockDeviceUID` (`config.plist:27`) — picks *which* clock B is, not how the buffers are sized.
- `NetworkInterface` (`config.plist:73`) — routing, not buffering.
- `AutoConnect` block — wiring topology only.
- `Logging.Level` — observability only.

---

## How the knobs interact

A few non-obvious couplings:

### P_pi and P_mac must match

`config.plist:33-36` documents this and `CLOCK_WARS.md` explains why:
netJACK2's master/slave handshake expects equal cycle sizes. If they
differ, netadapter's resampler throws `WriteResample error` on cycle 1
and thrashes. So you tune them as a pair, not independently.

### G interacts with measured network jitter

`-g 512` ≈ 11 ms of burst tolerance. JITTER.md §5 measured network
inter-arrival p99=2.7 ms, max=13 ms. So an extreme burst can still
overflow this ring. With the auto-restart sentinel in place, that
becomes "3 s of audio dropout, then recovery" rather than "permanently
degraded ring size for the rest of the session." Pick G to suit your
tolerance:

| G (frames) | Steady (ms) | Burst tolerance (ms) | Trade |
|------------|-------------|----------------------|-------|
| 256        | 2.67        | 5.33                 | **Current default.** Storm-restart is the safety net for the gap between this and measured network max (13 ms). |
| 512        | 5.33        | 10.6                 | Closer to measured p99 (2.7 ms) with ~4× headroom. |
| 1024       | 10.6        | 21.3                 | Comfortably above measured max jitter. |
| 2048       | 21.3        | 42.6                 | "Set and forget." |
| 4096       | 42.6        | 85.3                 | Studio session, no storm risk acceptable. |

### J (JitterFrames) is single-clock-domain only

Because the Mac is one clock end-to-end (CLOCK_WARS.md "where the SRC
actually lives"), J doesn't need a controller — it's just a constant
lead. Sizing it is purely about absorbing thread scheduling jitter on
the Mac (`docs/idiosyncrasies.md` — daemon's JACK thread vs HAL's IO
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

All Σ figures are **monitoring trip** (pi ADC → Mac → pi DAC).

**Current runtime behavior:**
- Pi sample rate and period are discovered at startup; Mac JACK receives the same values.
- Supported sample rates are 44100, 48000, and 96000 Hz.
- `JitterFrames=0`; no persistent timing keys are stored in the home plist.

Tune the Pi JACK period on the Pi itself. The next Mac startup probe follows
that value; the settings editor does not expose a period control.

**Diagnose where latency lives in YOUR setup:**
- Send a known transient (handclap, click track) from DAW to pi headphones, record back.
- The recorded delay is the monitoring trip ≈ Σ + your DAW's monitoring path latency.
- Bisect by changing one knob at a time and re-measuring.

---

## Where these numbers come from

- T_alsa, T_pj, T_mj, T_nm are from the JACK / ALSA buffer math (frames ÷ sample rate).
- T_g midpoint behavior is documented in jack2's `JackAudioAdapter::PushAndPull` — the controller targets midpoint via the resampler ratio.
- T_jf reasoning is in `jackbridge/installer/config.plist:40-48` and `docs/architecture.md`.
- Network latency default (`-l 2`, max 30) verified on the live pi by loading netadapter under a probe client name and reading the `Network latency : N cycles` line from `journalctl -u jack` (jack2 1.9.22 on Arch).
- T_adc / T_dac are codec group-delay values from the IQaudIO datasheet (low ms).
- ms numbers are computed for 48 kHz; scale for other rates.
