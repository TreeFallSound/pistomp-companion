# Verification runbook: the free-running cursors (send + recv)

Run this when the pi-stomp is back on the network. Everything below is
observation only except the two reload steps. Expected state: protocol v12
daemon and HAL driver built from this worktree (`just daemon` and `just driver`
succeeded), with the Pi and Mac JACK periods matched while the CoreAudio HAL
buffer remains independently selectable. No plugin changes.

## 0. Reload the stack

```sh
just install && just unlink-shm && just restart
```

Order matters (CLAUDE.md): the daemon must attach to a fresh region so the
new counters start at zero. Confirm attach:

```sh
just shm          # expect protocolVersion 12, jackPeriodFrames 64
```

If `just shm` shows a stale protocol version, the daemon is old — stop and
rebuild (`just daemon`) before continuing.

## 1. The headline test — clean-regime capture counters

Start REAPER **recording** from the JackBridge device (both directions
active) and let it run 60 s. Then capture:

```sh
just watch > /tmp/watch1.log &      # 2s cadence, ~30 samples
```

**Pass:** `dupWriteCycles` and `skipWriteFrames` **flat** across the whole
window. This is the defect this work closed — the v11/v12-era daemon showed
`dupWriteCycles` climbing ~2.2/s and `skipWriteFrames` ~416 frames/s in the
clean regime.

**Fail:** any steady climb in `dupWriteCycles`. Before diagnosing, check the
5 s health line for `sendResyncs=` — a nonzero steady rate there means the
rules are snapping; read `sendResyncs` first (DEBUGGING.md §3.3):

```sh
log stream --predicate 'subsystem == "com.treefallsound.companion" && category == "shm"' --style compact
```

A flat `dupWriteCycles` with occasional `sendResyncs` ticks is a pass with
an explanation; a flat one with zero snaps anywhere is a clean pass.

## 2. The xrun-stress test — counters through faults

Keep the recording running and stress the pi (load mod-host, add plugins —
whatever reliably produced xruns before). 60 s under stress, then compare
the same window.

**Pass:** `dupWriteCycles` stays flat *through* xrun windows. Xruns may move
`daemonXRuns` and the pi's `xruns_1m`, and `sendResyncs` may tick at the
xrun — that is the honest snap cost — but the write cadence itself holds.
This mirrors the recv side's verified behavior (flat through 36 xruns).

**Acceptable:** small `skipWriteFrames` increments that correlate 1:1 with
`sendResyncs` ticks (each snap costs its frames; DEBUGGING.md §3.3).
**Fail:** `dupWriteCycles` climbing independent of snaps.

## 3. The one-direction tests — the gate

These verify the head-moved gate, which the fuzz found and unit tests pin
but no live session has exercised.

**Playback-only:** in REAPER, play the JackBridge outputs with the input
device disarmed. 60 s.

**Pass:** `dupWriteCycles` **flat** (the input head never advances — no
ReadInput ops — and the gate must keep the send rule quiet). The v12 recv
rule failed the mirror of this in the fuzz at 6666 false snaps; the send
rule ungated would have produced ~327 in the same window.

**Recording-only:** arm the input, stop playback. 60 s.

**Pass:** `dupReadCycles` flat — this is the shipped v12 rule's defect
(the ungated recv rule false-snapped here); if it climbs in a steady
rhythm with `recvResyncs` ticking in step, the gate is not compiled in.

**Caveat when reading results (DEBUGGING.md §2.3):** in a one-direction
session the *idle* direction's counters read flat and prove nothing. Do not
read a flat idle-direction counter as a healthy capture path — it is flat
because nothing is happening there. Liveness is the heartbeat + anchor.

## 4. The renegotiation test — buffer-size bounce

With Chrome playing audio (or any second client joining the device), watch
the driver health line while REAPER resizes its buffer:

```sh
log stream --predicate 'subsystem == "com.treefallsound.companion" && category == "driver"' --style compact
```

**Pass:** through the 64→1024→32→512→64 bounce, no torn-write faults and
the counters record only resize-boundary costs. The fuzz's ungated recv
rule produced 1500 false snaps + 216,000 phantom `skipReadFrames` through
this bounce; the gated rules produced zero. Any nonzero counter movement
must correlate with a resize event in the driver log.

## 5. Latency sanity

```sh
just shm
```

**Expect:** `oneWayLatency` 1106 frames @ P=64/48k, `jitterFrames` 128. The
cursor work adds no latency — verify nothing moved by comparing against
`docs/LATENCY-MODEL.md`'s deployed-recipe block.

## 6. If something fails

- A steady `sendResyncs`/`recvResyncs` rate with counters flat is a
  clock-rate signature — DEBUGGING.md §6 (not a cursor bug).
- `dupWriteCycles` climbing with `sendResyncs` flat is a cursor walk bug —
  capture `just watch` at 2s for 5+ minutes plus the shm os_log stream and
  stop; that combination is the reproduction.
- Do not hand-tune `JitterFrames` to mask a failure; J is a measured
  cushion, not a correction knob (LATENCY-MODEL.md "J is now load-bearing").

## 7. After the live pass

- The HAL buffer cap and independent 512-frame default are implemented. A
  coreaudiod restart is required for hosts to observe the new default and the
  32..1024 range. Confirm the active N through the host or driver log.
- The remaining driver simplification is the syncMode-0 timestamp cleanup;
  do not land it without the bootstrap and daemon-death checks described in
  the plan.
- Optionally revisit J sizing with the new counters (the J=128-vs-maxBurst
  question in LATENCY-MODEL.md) — that is a measurement campaign, not a
  code change.