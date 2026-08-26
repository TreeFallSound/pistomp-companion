# PI controller integrator windup, never reset

In jack2 1.9.22:

- PI controller setpoint IS midpoint (JackResampler.h:68-71: error = read_space - size/2). So LATENCY-MODEL.md's "G/2" is the right nominal number.
- On ring failure, ResetRingBuffers() re-centers ring to G/2 AND clears libsamplerate filter state (JackResampler.cpp:45, JackLibSampleRateResampler.cpp:76).
- But it does NOT touch JackPIControler::offset_integral (JackFilters.h:325). The integrator is only zeroed in the constructor.
- JackPIControler::OurOfBounds() exists at JackFilters.h:333-344 and is exactly the routine that would clear the integral — it has zero call sites in jack2 1.9.22. Dead code. Known-but-unfixed.
- Output ratio is clamped to [0.25, 4.0] but the integral itself is unclamped — classic missing-anti-windup.
- -g 256 disables adaptive ring growth (JackNetAdapter.cpp:124-126), which would otherwise hide this by doubling ring size each failure until it stops failing.

## Why every symptom fits

```
┌───────────────────────────┬────────────────────────────────────────────────────────────────────────────────────┐
│        Observation        │                                    Explanation                                     │
├───────────────────────────┼────────────────────────────────────────────────────────────────────────────────────┤
│ Monotonic failure ramp    │ Integral grows unbounded across resets → ratio drifts further from true → ring     │
│                           │ hits wall sooner → more error feeding the integral → ramps                         │
├───────────────────────────┼────────────────────────────────────────────────────────────────────────────────────┤
│                           │ Latency is dominated by network double-buffering + soft buffers + JACK period (all │
│ Latency stays ~constant   │  fixed). Ring fill position doesn't affect bytes-in-flight; only headroom-to-wall  │
│                           │ (H3 confirmed)                                                                     │
├───────────────────────────┼────────────────────────────────────────────────────────────────────────────────────┤
│ Restart = clean slate     │ JackPIControler constructor zeroes offset_integral. Only way to clear it without a │
│                           │  source patch                                                                      │
├───────────────────────────┼────────────────────────────────────────────────────────────────────────────────────┤
│ 2:1 consumer-vs-producer  │ Direction of windup-driven ratio bias — playback ring drifts toward "full" wall    │
│ asymmetry                 │ ~2× faster than capture drifts toward "empty"                                      │
├───────────────────────────┼────────────────────────────────────────────────────────────────────────────────────┤
│ 4,800 ppm "drift" I       │ Not drift. That's the integrator-driven ratio bias sailing the ring; real crystal  │
│ computed earlier          │ skew is 10–100 ppm and is what feeds the integrator, but the failure rate is set   │
│                           │ by the controller pathology, not the skew                                          │
└───────────────────────────┴────────────────────────────────────────────────────────────────────────────────────┘
```

## What this means for our options

Three viable fixes, ordered by effort:

1. Periodic netadapter reload (cron-style). Zero code. Audible glitch every N minutes. Confirms the diagnosis if the ramp resets cleanly each cycle.
2. Patch jack2 to call OurOfBounds() (or just zero offset_integral) in ResetRingBuffers. ~3-line patch in JackAudioAdapterInterface.cpp. Rebuild jack2
on the pi. The actual fix.
3. MAC-NETADAPTER flip. Moves the same bug to the Mac. Doesn't fix it — just changes where it manifests. Demote this until #2 is tested; the Mac has more CPU but the bug is in the controller, not in CPU budget.

Also: LATENCY-MODEL.md's T_g = G/2 claim is correct as a nominal value but should add a note that under windup the steady-state fill can drift off setpoint without changing measured latency — only failure margin.
