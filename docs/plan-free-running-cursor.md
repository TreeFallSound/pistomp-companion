# Plan: a free-running read cursor — both directions

This plan removes the last cadence defect on the playback path, and then
the mirror defect on the capture path. Read `docs/DEBUGGING.md` section 6
for the measurement that found the first one.

---

> **Status: implemented, both directions.**
>
> **Recv (playback) side:** the cursor, `recv_target()`/`recv_error()`, the
> `RECV_RESYNCS` counter and the unit-test harness
> (`jackbridge/daemon/RingProjector_test.cpp`, `just test-projector`) all
> landed. One deviation from this plan, found by the test: the forward
> window edge is `block + jitter − period`, not `jitter`. Between HAL head
> jumps the cursor legitimately runs up to `block − period` ahead of the
> target; a tighter edge snaps mid-walk and re-reads zeroed slots every few
> cycles. Section 4's table is superseded by the window in
> `RingProjector.hpp` and in `check_progress()`.
>
> **Send (capture) side:** implemented as the exact mirror — `mSendCursor`,
> `send_target()`/`send_error()`, replacing the delta-term `send_offset()`
> and `mLastSyncedReadFrame` entirely. The window differs from recv in one
> deliberate way: **no forward discipline edge.** A stalled consumer is
> absorbed open-loop (the delta term's one good behaviour, kept); the only
> snaps are the two hard hazards:
>
>   torn edge:  `err < N − B − J`    (write would land in the consumer's
>                                     live block [H, H+N); N is the TRUE
>                                     HAL block, not max(N, P))
>   lap edge:   `err > ring − B − J − P`  (write would wrap a full ring
>                                     onto it)
>
> Snaps are counted in `mSendResyncs`, published on the 5 s health os_log
> line (`sendResyncs=`/`recvResyncs=`) — no shm slot was free before
> `STRBUF_U0`, and the audio cost already lands in dupWrite/skipWrite.
>
> **The head-moved gate (both rules, found by session-shape fuzz):** each
> snap rule fires only on cycles where its head published a new position.
> A head that never moves means no IO op ran in that direction — a
> playback-only session never runs ReadInput, a recording-only one never
> runs WriteMix — so there is no live block to collide with and the
> open-loop walk is correct. Ungated, the rules snapped against frozen
> heads in a steady rhythm: the v12 recv rule produced a phantom
> clock-rate error (~6666 false snaps in a 27 s recording-only window)
> and the send rule would have done the mirror in playback-only. With
> the gate: zero false snaps, and a clock-tracked resume preserves the
> walk's error so a paused direction costs nothing on the way back.
>
> **Error representation (both sides):** plain absolute-frame differences.
> The send lap edge (`ring − B − J − P ≈ 3840` at the live config) lies
> beyond ring/2, where a shortest-way-around-ring comparison folds negative
> and inverts the snap direction — the mid-implementation simulation caught
> exactly that. ALSA and WASAPI solve the same problem the same way:
> unbounded position counters, ring offset taken by `%` at the point of
> use, never by folding the comparison.
>
> **Cadence counters:** now measured as signed absolute deltas between
> consecutive cursor positions. The old masked-position gap could not represent
> a backward snap — it read as a huge forward skip of `ring − k`. Forward snap
> displacement lands in the skip-frame counters; backward snaps increment the
> dup-cycle counter, which does not retain their magnitude.
>
> **Degenerate N=2048:** ring = 2N leaves the send window with exactly the
> walk's sawtooth amplitude (N−P) and zero margin — any one-frame
> perturbation snaps. Cap `kMaxIOBufferFrames` at 1024 (driver note; the
> plugin side needs a coreaudiod restart, deliberately not done here).
>
> The hardware measurement of section 5 still needs to be taken against
> the four N/P ratios.

## 1. The defect

`recv_offset()` derives the read position from the HAL's write head every
cycle. It walks forward through the settled block, and it clamps the walk at
`block_frames - period_frames`.

The clamp is the defect. N JACK cycles and one HAL cycle do not divide evenly
in time. When the walk reaches the clamp and the head has not advanced yet,
the position repeats. The daemon then reads a slot that
`ring_consume_stereo_interleaved` already zeroed, and it sends silence. On the
next cycle the head advances, and the position jumps two periods.

Each event costs one period. Measured at N = 512 and P = 64: 12 events in
16 s, which is 768 lost frames, or 0.1% of the audio.

## 2. Why a free-running cursor is safe here

The daemon may hold its own cursor because both sides share one clock.

`jackd-launch` starts jackd with the **coreaudio** backend on the built-in
output device (`jackbridge/installer/jackd-launch:262`). `syncMode` is 1, so
the daemon publishes the timeline and the HAL relays it. The HAL's IO proc
rate therefore derives from the same crystal that drives the daemon's JACK
cycle.

The two rates match over the long term. A cursor that advances by `nframes`
each cycle stays in phase. It needs a correction only for jitter, not for drift.

## 3. The change

Four steps. (The original plan stopped at the recv side and left
`send_offset()` alone; the capture side later developed the same defect —
`dupWriteCycles` climbing at 2.2/s in the clean regime — and gained the
mirror cursor. See the status header.)

**Step 1. Add the cursor.** Add `uint64_t mRecvCursor{0}` beside
`mLastSyncedWriteFrame` (`jackbridge/daemon/JackBridge.cpp:716`). The cursor
counts absolute frames. Mask it to the ring only at the point of use.

**Step 2. Give `RingProjector` the target and the error.** Keep
`recv_offset()` as it is, and rename it `recv_target()`. It becomes the
position the cursor should hold, not the position the daemon reads. Add:

    int64_t recv_error(uint64_t cursor) const;   // cursor - target, signed

Drop `walk_frames()`. The cursor replaces the walk.

**Step 3. Advance the cursor in the process callback.** Read at
`mRecvCursor & (ring_frames - 1)`. Then add `nframes`. Do this once per cycle,
in `receiveFromCoreAudio`.

**Step 4. Seed and reset the cursor.** Set `mRecvCursor` to the target on the
first active cycle, and again in `reanchor()`
(`jackbridge/daemon/JackBridge.cpp:946`). The cursor is a timeline position,
so it moves with the anchor.

## 4. The resync rule

Correct the cursor only when it leaves a safe window. Apply the rule in
`check_progress()`, before the copy paths run.

The window is not symmetric. Forward is the tight side, because the block the
HAL is writing lies at `W`. Backward is loose, because a whole ring lies
behind.

| Condition | Action |
|-----------|--------|
| `recv_error > jitter_frames` | The cursor approaches the live block. Snap to the target. |
| `recv_error < -(block_frames)` | The cursor falls too far behind. Snap to the target. |
| otherwise | Free-run. Do not touch the cursor. |

Count every snap. Add a `recvResyncs` field, or reuse `dupReadCycles` for a
backward snap and `skipReadFrames` for a forward snap. Do not let a snap pass
unrecorded; a silent correction is how a rate error hides.

A HAL stall needs no separate case. The head stops, the error grows, the rule
snaps the cursor back, and the behaviour degrades to the current clamp.

## 5. How to test

Use the method in `docs/DEBUGGING.md` section 4. Play a tone, take the
counters, wait a known time, take them again.

Test each ratio. N is set by the host, so drive it with a host you can
configure, or with `afplay` and a DAW at different buffer sizes.

| N | P | Expected |
|---|---|----------|
| 64 | 64 | all four counters flat |
| 512 | 64 | all four counters flat |
| 1024 | 64 | all four counters flat |
| 64 | 512 | all four counters flat |

The pass condition is stronger than today's. `dupReadCycles` must stay at
zero, not merely low. A nonzero count means the rates do not match, and
section 6 applies.

Extend the harness in `RingProjector`'s unit test first. The projector stays
pure, so the cursor logic can be tested without JACK and without shm.

## 6. The risk to check

The plan assumes the two rates match exactly. Section 2 gives the reason to
believe this. Measure it before you trust it.

If the rates differ, the cursor drifts, and the rule snaps it at a fixed
period. The defect then changes shape: it becomes rarer and larger, not
absent. Watch `recvResyncs` over ten minutes. A steady rate means a rate
error, and a rate error needs a different fix.

Do not add SRC to repair this. `CLAUDE.md` rule 3 holds: both sides share the
CoreAudio clock, and netJACK2 owns the cross-clock resampling.

## 7. Driver-architecture simplifications enabled (notes only)

The cursor work clarified two driver-side simplifications. Neither is
implemented here — both touch the plugin (`SA_Device.cpp`), which requires
a coreaudiod restart to reload, and the device was not available.

1. **Cap `kMaxIOBufferFrames` at `(STRBUFNUM/2)/4 = 1024`**
   (`SA_Device.cpp:87`, currently `(STRBUFNUM/2)/2 = 2048`). At N = 2048
   the ring is exactly 2N, and the send window
   `[N−B−J, ring−B−J−P]` has width exactly equal to the steady walk's
   sawtooth amplitude N−P: zero margin, so any one-frame perturbation —
   a late head observation, a single missed cycle — snaps. The old
   delta-term `send_offset()` had the same geometry and would have torn
   there too; the cursor merely makes the arithmetic visible
   (`test_send_window_no_margin_at_N2048` pins it). At N = 1024 the
   window has 2048 frames of margin over the walk — the cap costs
   nothing legitimate and removes an unsupportable configuration.

2. **Delete the dead syncMode-0 path in `SA_Device::GetZeroTimeStamp`**
   (`SA_Device.cpp:1679-1683`). `isSyncMode` is hardcoded `true` in the
   daemon (`JackBridge.cpp:226`, FIXME noted there), so `shmSyncMode` is
   always 1 once the daemon activates, and the driver's else-branch is
   unreachable in practice. Removing it also retires the
   `gDevice_AnchorHostTime` / `gDevice_NumberTimeStamps` /
   `theHostTicksPerRingBuffer` maintenance that feeds only that branch
   (1580, 1666-1672, 2106) — while keeping the `theHostTicksPerRingBuffer`
   value itself, which the daemon-heartbeat watchdog threshold at 1657
   also uses. No shm layout change, so no protocol bump.

Both are pure simplifications — simpler software has fewer bugs — and both
should land as one driver commit the next time a coreaudiod restart is
convenient.
