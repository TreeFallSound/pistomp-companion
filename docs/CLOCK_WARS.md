# Clock Wars

Status as of 2026-05-31. This doc names the actual enemy behind the
clicks in JITTER.md: **JackBridge is trying to bridge two independent
sample clocks, and there is no buffer large enough to make that
bit-exact over the long run.**

---

## The impossibility result, in one paragraph

The Pi's IQaudIO codec ticks off the BCM2711 I2S clock, derived from a
PLL on the Pi SoC. The Mac's CoreAudio device ticks off whatever crystal
clocks `BuiltInSpeakerDevice` (or whichever device `ClockDeviceUID`
points at). These are two separate physical oscillators on two separate
boards. Even with perfect crystals (~±20 ppm), they drift relative to
each other — at 48 kHz, 20 ppm is ~1 sample/second of relative skew.
Over any session longer than `buffer_size / drift_rate` seconds, the
faster clock produces more samples than the slower one consumes (or
vice versa). The buffer between them *must* eventually under- or
overflow, no matter how large it is.

To bridge them without a glitch, the Pi-side **netadapter** resamples
— which by definition is not bit-exact, because the output samples are
interpolated estimates of where the source would have been at the
destination's clock instants. Even at 99.9% match, you're
reconstructing, not transporting.

**Conclusion: bit-exact pi↔Mac audio transport is impossible under
the current hardware topology.** Not "hard" — impossible.

### Where the SRC actually lives

It's worth being precise about *which* component does the resampling,
because it shapes how the rest of the system can be reasoned about:

```
[ pi codec, clock A ] → jackd → netadapter ───UDP cycles───> netmanager → jackd → daemon → shm → HAL → [ Mac CoreAudio, clock B ]
                                    ▲                              ▲
                                    │                              │
                          JackLibSampleRateResampler        no SRC — just packet I/O
                          (the only SRC in the path)
```

- **Pi netadapter (slave)** holds the slip ring + `JackLibSampleRateResampler`
  that bridges clock A to the netjack cycle clock. *All* sample-rate
  conversion happens here.
- **Mac netmanager (master)** drives the netjack cycle from clock B and
  receives/sends packets. It does **no** resampling.
- **Mac jackd → JackBridged → HAL → CoreAudio** is therefore a single
  clock domain (clock B), end to end. That's why `JitterFrames` is a
  one-sided safety margin, not a slip ring with a controller — there
  is no drift to absorb on the Mac side, only scheduling jitter.

So clock B is the master of the whole pipeline; clock A slaves to it
via one resampler, located on the pi. The "two clocks" are real but the
bridging is one-sided. Tuning `-g` and `-q` on netadapter is therefore
tuning *the entire* cross-clock bridge.

---

## What's actually happening today

| Symptom in JITTER.md          | Root cause in clock terms                          |
|-------------------------------|----------------------------------------------------|
| Ring "producer/consumer too slow" → doubling | Slip ring underran because fast side outran slow side faster than the resampler could compensate |
| Audible clicks at doubling    | Ring reset is a discontinuity in the resampler state |
| Inter-arrival jitter (p99 2.7 ms, max 13 ms) | Network jitter on top of clock skew — the resampler has to absorb both |
| Clicks with empty pedalboard  | Even with zero plugin load, the two clocks still drift; resampler still has to work |

The clicks aren't a bug we can fix in code. They're the audible
manifestation of the clock mismatch. We can make them rarer (bigger
buffer, better resampler) but not eliminate them without changing the
clock topology.

---

## The four ways to get one clock

Ranked by how realistic they are for *this* hardware (Pi 5 + IQaudIO
codec + Mac via Ethernet):

### 1. Stay with two clocks, just tune harder (current path)

Accept that bit-exactness is impossible. Pick buffer sizes and
resampler quality that make the resampling artifacts inaudible and
the underruns rare enough to not matter for the use case (DAW
recording, ~hours-long sessions).

This is what JITTER.md's "what's left to try" is really about. The
ring doublings are audible because the ring *starts small* and grows
on failure — pinning it big (`-g 2048`) hides the learning phase. The
resampler is already running; making it cheaper (`-q 0`) frees CPU at
some cost to fidelity (which we can't measure-bit-exact anyway, so the
trade is essentially free for our use case).

**Cost:** four-line config change.
**Buys us:** the current architecture, working acceptably.
**Doesn't buy:** bit-exactness, ever.

### 2. USB Audio Class 2 gadget mode

Pi exposes itself as a UAC2 device to the Mac. The Mac's USB host
provides isochronous SOF timing; the Pi's gadget driver slaves the
codec to SOF. **One clock end-to-end.**

**Blocked on this hardware.** Pi 5's USB-C is power-only at the silicon
level — the data pins aren't wired to a USB controller, only to PD
negotiation. The four USB-A ports hang off the RP1 southbridge, which
is host-only. No OTG silicon exposed anywhere on a Pi 5. (Pi 4 *would*
work — its USB-C is wired to the `dwc2` controller. But pi-stomp uses
Pi 5.)

A future hardware revision with a PCIe HAT that includes a
device-mode-capable USB controller could unblock this.

### 3. AVB (IEEE 802.1) with hardware-assisted clock recovery

Real pro-audio approach. Switch wire format to AVB (L2 Ethernet, no
IP, uses gPTP for clock sync). The Pi 4/5's `bcm54213pe` PHY supports
IEEE 1588 hardware timestamping, so the *timestamp* side works. The
problem is what to do with the recovered clock:

- A *real* AVB endpoint has a VCXO (voltage-controlled crystal
  oscillator) feeding the codec MCLK, steered by a DAC driven by
  PTP-error in firmware. Codec literally ticks in lockstep with
  gPTP. One clock, bit-exact.
- This board has no VCXO. The codec is I2S-slave to a BCM2711 PLL
  that's tunable in software but with no electrical feedback loop
  from the NIC's PTP unit. So we'd be running a *software* DPLL —
  reading PTP error in userspace, nudging the PLL via sysfs. That's
  better than free-running but still has a many-millisecond control
  loop and quantized PLL steps. **Effectively just a more elaborate
  resampler.**

**Cost:** rewrite of the network layer (AVB stack instead of netJACK2),
plus a software DPLL on the Pi.
**Buys us:** marginally better clock alignment than netJACK2's
resampler, *not* bit-exactness.

### 4. Daughterboard with VCXO

Add a small board: VCXO clocking the codec MCLK, DAC steering the
VCXO, fed by PTP error from the NIC. This is a real AVB endpoint.
Bit-exact, by construction.

**Cost:** hardware design + fab + firmware. Real engineering project.
**Buys us:** bit-exactness. The actual fix.

---

## What we're choosing, and why

For now: option 1. We don't actually need bit-exactness for the use
case — DAW recording with monitoring is fine at 24-bit/48k with
inaudible resampling artifacts, and we can't perceive sub-sample
artifacts on guitar/vocal signals anyway.

The clicks aren't from resampling fidelity — they're from ring
under/overruns when drift + jitter exceed buffer headroom. Pinning the
ring big (`-g 2048`+) and matching periods (Pi `-p 128`, Mac
`PeriodFrames 128`) buys enough headroom that the slip ring stays
mid-fill across realistic drift + network jitter. That's the actual
fix for the audible problem, even though it doesn't address the
philosophical one.

If we ever care about true bit-exactness — for measurement, for null
tests, for archival recording — the answer is option 4. There is no
option 4-lite.

---

## Things to remember when this comes up again

- "Why can't we just make the buffer bigger?" Because drift is
  unbounded over time. A bigger buffer buys time, not freedom.
- "Why does netJACK2 resample if both ends are 48 k?" Because "48 k"
  is the nominal rate of two physically separate crystals. Their
  actual rates differ by ~tens of ppm and drift with temperature.
- "Can we just sync the clocks over the network?" That's PTP. PTP
  syncs *timestamps*. To sync the *audio clock*, you need hardware
  that can be steered by PTP error — a VCXO or equivalent. The Pi
  doesn't have one for its codec.
- "Could we resample to a much higher rate and just truncate?"
  No. Resampling at any rate is interpolation; it's not bit-exact at
  the source rate either. The only way to get bit-exact is for the
  output samples to be taken at integer multiples of the same clock
  the input was sampled on.
