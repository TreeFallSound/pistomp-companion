# Plan: correctness repairs for the free-running cursors

Findings from the review of the staged cursor work. Read
`docs/plan-free-running-cursor.md` for the design and `docs/VERIFY-CURSORS.md`
for the live run these repairs unblock.

Each item gives the evidence, the repair, and the reason.

---

## 0. The order of the work

Do item 1 before item 2. Item 2 hides item 1.

The misfiring divergence detector calls `reanchor()` every 15 s.
`reanchor()` writes `ZERO_HOST_TIME` and `NUMBER_TIMESTAMPS`, which is the
anchor the deleted publish wrote. The stack therefore still tracks the
daemon timeline, at a 15 s cadence with a seed bump, instead of a smooth
85 ms cadence.

If you repair item 2 first, the re-anchors stop and no writer remains. The
anchor then freezes for the whole session, the drift becomes unbounded, and
the symptom looks like the clock-rate error of `plan-free-running-cursor.md`
section 6. You will look for a fault that is not there.

Groups A and B are blockers for the live run. Groups C and D are not.

---

## A. Blockers

### A1. Restore the timeline publish

**Evidence.** The staged diff removes this block from the process callback
(`JackBridge.cpp`, HEAD:491-496):

```cpp
if ((FrameNumber % RingFrames) == 0) {
    shmZeroHostTime->store(mach_absolute_time(), std::memory_order_relaxed);
    shmNumberTimeStamps->store(FrameNumber / RingFrames, std::memory_order_release);
}
```

The remaining writers are `:470` (activation, stores 0) and `:988`
(`reanchor`).

**Why it is wrong.** `syncMode` is 1. In that mode the driver only relays the
two fields (`SA_Device.cpp:1675-1677`). No writer means no timeline. CoreAudio
then extrapolates from one stale anchor at the nominal rate.

**Repair.** Put the block back. `RingFrames` equals `mRingBufferFrameSize`
(both are `STRBUFNUM/2`), so `outSampleTime` equals `FrameNumber` exactly. The
anchor states "JACK frame F occurred at host time T".

**Check.** `just shm` twice, 1 s apart. `numberTimeStamps` must increase.

### A2. Wire the anchor-advance comparison

**Evidence.** `mCachedHalAnchorSample` (`JackBridge.cpp:757`) has no writer.
`halAnchorSample` loads into a local in the seqlock snapshot (`:1027`,
`:1034`) and dies there. `mLastHalProgressFrame` (`:760`) is written only by
`reanchor()` (`:998`).

**Why it is wrong.** `delta` at `:1227` becomes "frames after the last
re-anchor". It passes `kReanchorThresholdFrames` (4096) in 85 ms. Three
windows later the daemon re-anchors. The daemon re-anchors every 15 s on a
healthy stack. `healthDeltaMax` and `healthSnaps` carry no information.

**Repair.** Add the comparison the comment at `:748-756` describes:

```cpp
if (halAnchorSample != mCachedHalAnchorSample) {
    mCachedHalAnchorSample = halAnchorSample;
    mLastHalProgressFrame  = FrameNumber;
}
```

The HAL writes the anchor on every IO op in either direction
(`SA_Device.cpp:1763`), so the delta stays bounded in a one-direction
session. This is what keeps re-anchors rare.

**Check.** `just logs` must show no `timeline diverged` line on a healthy
stack. `reanchorCount` in `just shm` must hold still.

---

## B. Correctness

### B1. Guard the degenerate geometry

**Evidence.** Simulation against the real header, 20000 cycles for each
configuration:

| N | P | J | 2·B+J | ring | snaps |
|---|---|---|-------|------|-------|
| 512 | 64 | 128 | 1152 | 4096 | 0 |
| 1024 | 64 | 128 | 2176 | 4096 | 0 |
| 2048 | 64 | 128 | 4224 | 4096 | **1249** |
| 2048 | 64 | 0 | 4096 | 4096 | 0, no margin |
| 64 | 512 | 128 | 1152 | 4096 | 0 |
| 64 | 2048 | 128 | 4224 | 4096 | **seed outside the window** |

`B` is `block_frames`, which is `max(N, P)`.

Two failures, one cause.

**The N family.** The walk peak is `N−P` = 1984. The lap edge is
`R−B−J−P` = 1984−J. The peak passes the edge by exactly J. The daemon snaps
one time for each head group. J removes margin here, so `J = 0` is the only
value that works at N=2048. An operator who raises J to repair a click makes
this configuration worse.

**The P family.** At N=64, P=2048, J=128 the window is `[−2112, −128]`.
`send_target()` gives an error of 0, which is outside that window. The snap
does not converge: `mSendCursor += −sendErr` with `sendErr = 0` moves the
cursor 0 frames, and the rule fires again on the next cycle with a moved
head. `mSendResyncs` climbs at the cycle rate and the cursor never changes.
The deferred `kMaxIOBufferFrames` cap does not cover this, because jackd sets
P.

**Repair.** One inequality covers both families:

```
ring_frames > 2 * block_frames + jitter_frames
```

Derivation. For N ≥ P the walk must fit below the lap edge:
`R − N − J − P > N − P`, so `R > 2N + J`. For P > N the target must lie
inside the window: `R − P − J − P > 0`, so `R > 2P + J`. `block_frames` is
`max(N, P)`, so one test covers both. The predicate matches the simulation at
every point, and at the equality edge.

At J=128 the guard gives `max(N, P) < 1984`.

Apply the guard in the daemon, where N or P changes. Log one line and raise a
fault bit. Do not clamp J: a silent J change moves the advertised latency and
breaks `LATENCY-MODEL.md`. Fail loud — `CLAUDE.md` rule 4.

**Also repair the test.** `test_send_window_no_margin_at_N2048:429` compares
the window *width* with `N−P`, and `<=` passes at equality. Width is not the
property. Assert that the walk fits: `walk_peak < send_forward_limit`.

### B2. Give `reanchor()` the JACK period

**Evidence.** `JackBridge.cpp:995-997` call `block_clearance(0)`.
Pre-existing at HEAD on the recv line; the staged send line mirrors it.

**Why it is wrong.** `block_clearance` returns `max(mCachedHalNFrames, 0)`,
so it drops P. If P > N, the re-seeded recv cursor reads `P − N − J` frames
past the write head until the next snap.

**Repair.** Pass `nframes` to `reanchor()`. Every caller is on the process
callback, where `nframes` is in hand.

### B3. One copy of the window rules

**Evidence.** `RingProjector_test.cpp:79-90` and `:241-256` hold the limits.
`JackBridge.cpp:1099-1101` and `:1147-1154` hold them again.

**Why it is wrong.** The windows are the whole change. Two copies can drift,
and the test then passes while the daemon is wrong.

**Repair.** Move `recv_forward_limit`, `recv_backward_limit`,
`send_forward_limit`, `send_backward_limit` and the two `outside_window`
predicates into `RingProjector.hpp`. The daemon and the tests call the same
definition.

---

## C. Honest counters and comments

### C1. The dup counters lose the magnitude

`JackBridge.cpp:1188` and `:1198` add 1 for each cycle with `gap < period`.
`DEBUGGING.md` section 3.1 says a snap cost lands "at its true magnitude".
That is true for the skip counters only.

Repair the claim in `DEBUGGING.md`, or record the deficit in frames beside
the event count. The document must not promise what the counter does not
carry.

### C2. The `mSendResyncs` comment gives a false reason

`JackBridge.cpp:723` says the control block has no free slot before
`STRBUF_U0`. `JB_OFF_RECV_RESYNCS` is `0x2a0` and `STRBUF_U0` is `0x10000`,
so 64 KB is free. The true cost is a protocol bump plus `ShmReader.swift`.

Correct the comment. Give `sendResyncs` a slot at the next bump: today the
menu bar and `just shm` show `recvResyncs` but not its mirror, and
`VERIFY-CURSORS.md` section 1 needs an `log stream` that the other four
counters do not.

---

## D. Documents

- `VERIFY-CURSORS.md` section 0 says "protocolVersion 0x130". `0x130` is
  `JB_OFF_PROTOCOL_VERSION`. The value is 12 (`JackBridge.h:45`), and
  `jbdump.cpp:87` prints it in decimal. The operator stops at step 0 each
  time. Change it to 12.
- `JackBridge.h:313-315` says `J = 0` costs a full ring, and "Keep it ≥ 1".
  `LATENCY-MODEL.md` says `J = 0` is the tightest correct alignment. One is
  stale. Settle it, and state the send-side floor from B1 in the same place.
- `LATENCY-MODEL.md` T_mj row: restored.

---

## E. `syncMode` — what can go, and what cannot

**The mode can go. The bootstrap anchor cannot.**

Mode 1 is correct and must stay the operating behaviour. The virtual device
has no crystal, and jackd runs on a real hardware clock device
(`jackd-launch:262`, `-P "$CLOCK_UID"`). Mode 1 anchors the virtual device to
that same crystal, so the DAW IO proc and the daemon ring writes hold one
oscillator. Mode 0 builds the timeline from `mach_absolute_time()`, which is
a different oscillator, and the ppm difference is exactly the rate error the
cursor cannot absorb and rule 3 forbids repairing with SRC.

Mode 0 as an *operating* mode has never shipped. Delete that idea.

Mode 0 as a *bootstrap* path is live and necessary. Both sides write 0 when
they start (`SA_Device.cpp:2037`, `JackBridge.cpp:231`), and the daemon
claims the timeline only on its first active cycle (`:469`). Before that, a
DAW can open the device with jackd down, and `GetZeroTimeStamp` must still
return an advancing anchor. `plan-free-running-cursor.md` section 7 item 2
calls this branch dead. It is not dead. Delete it and the driver relays a
host time of 0 to coreaudiod.

**Recommended replacement.** Select on anchor freshness, not on a stored
mode. `ZERO_HOST_TIME` is itself a host time, so freshness needs no new
field:

```
fresh = (now - shmZeroHostTime) < 5 * theHostTicksPerRingBuffer
fresh   -> relay the daemon anchor
stale   -> self-anchor locally, do not publish
```

This deletes the flag, keeps the bootstrap, and repairs a hazard the flag
never covered: a daemon that dies mid-session currently leaves the driver
relaying a frozen anchor for as long as the session lasts. With A1 restored
the publish cadence is 85 ms, so a threshold of five ring buffers (~426 ms)
is safe.

Keep the `SYNC_MODE` offset and keep publishing it for `jbdump`. Stop reading
it as a selector. Reclaim the slot at the next protocol bump.

This is a driver change. It needs a coreaudiod restart, and it changes the
bootstrap path, so do not land it blind.

---

## F. Deferred driver work

`plan-free-running-cursor.md` section 7, corrected:

1. **Cap `kMaxIOBufferFrames` at 1024.** Still correct, and B1 gives the
   reason in one line: at J=128 the guard needs `max(N, P) < 1984`. The cap
   bounds N only. B1's daemon-side guard bounds P.
2. **Delete the syncMode-0 path.** Wrong as written. See section E.

---

## G. Verification

Run `docs/VERIFY-CURSORS.md` after group A and group B.

Section 3 of that runbook cannot pass before A2. `reanchor()` re-seeds both
cursors from the cached heads, and one cache is frozen in a one-direction
session, so each forced re-anchor moves the idle cursor backward and
`:1188` records one dup event. After A2 the re-anchors become rare, and one
bounded dup event for each genuine re-anchor is honest.
