# Debugging JackBridge

This document tells you how to read the diagnostics. It does not tell you
which command to run — `CLAUDE.md`, section "Observe and recover", does that,
and it explains why `just shm` and `just logs` answer different questions.

This document is for the maintainer. It is not for the user. A user has the
menu bar and nothing else. `CLAUDE.md`, section 4b, holds that rule.

---

## 1. The two clocks you must separate

Almost every reading here depends on two frame counts. They are independent.
Confusing them causes wrong diagnoses.

| Name | `just shm` field | Who sets it | Range |
|------|------------------|-------------|-------|
| **P**, the JACK period | `jackPeriodFrames` | the pi, through jackd-launch | 64 today |
| **N**, the HAL block | `halNFrames` | the CoreAudio host (the DAW) | 32…2048 |

The host sets N through `kAudioDevicePropertyBufferFrameSize`. Nothing
reconfigures the ring when the host does this. A DAW at 1024 and a pi at 64
is a normal, supported combination, and the ratio N/P is then 16.

Read both fields before you read anything else. Many symptoms below are a
function of N/P, and a reading taken without N/P is not repeatable.

---

## 2. How to read `just shm`

The fields are grouped below by the question each group answers. `jbdump`
prints a warning after a field when the value is bad, so a plain line is a
healthy line.

### 2.1 Is the stack up?

| Field | Healthy | What a bad value means |
|-------|---------|------------------------|
| `protocolVersion` | equals the header | The daemon and the driver are different builds. Run `just rmshm`. |
| `driverStatus` | `2 (STARTED)` | `1 (ACTIVE)` means no host holds the device. This is correct when nothing plays. |
| `driverFault` | `0 (none)` | `DEVICE_NOT_ALIVE` means the driver feeds silence; `BAD_RING_GEOMETRY` means the current N/P/J cannot fit safely in the ring. |
| `daemonAlive` | advances | A frozen counter means the daemon died or blocked. |
| `slavePortsConnected` | `6 of 6` | `0 of 6` means the pi's netadapter is absent. Read the pi's journal. |
| `deviceName` | the pi's name | An empty name means no daemon has attached. |

`driverStatus` deserves care. The daemon returns early and writes silence
whenever the status is not `STARTED`. Start playback before you measure
anything, or you measure an idle stack.

### 2.2 Is the timeline holding?

| Field | Healthy | What a bad value means |
|-------|---------|------------------------|
| `healthDeltaMax` | `0 frames` | The HAL stalled. The value is the deepest stall in the last 5 s window. |
| `healthSnaps` | `0` | The stall passed the snap threshold. |
| `reanchorCount` | stable | A climbing count means the timeline diverges and cannot converge. |

Read these fields before and after every measurement. A diverged timeline
reports `STARTED`, no fault, a live heartbeat and 6 of 6 ports while the audio
is noise. It voids the measurement silently.

One blind spot: `healthDeltaMax` compares the HAL's head against the
daemon's own `FrameNumber`, and both freeze together when it is the daemon's
process callback that stalls. A daemon-side stall therefore reads as a clean
window here. It still shows — as a `recvResyncs` snap in section 2.3, with
`skipReadFrames` absorbing the frames that arrived while the callback was
not running — so read that field before concluding the timeline held.

### 2.3 Is the ring cadence correct?

These five fields are the subject of section 3. They are the ones that catch
a period mismatch.

| Field | Healthy | What a bad value means |
|-------|---------|------------------------|
| `dupReadCycles` | flat | The daemon read a slot it already consumed. The pi got silence. |
| `skipReadFrames` | flat | The daemon stepped over frames the HAL wrote. Nothing read them. |
| `dupWriteCycles` | flat | The daemon wrote the same ring position twice. |
| `skipWriteFrames` | flat | The daemon left ring positions unwritten. |
| `recvResyncs` | flat | The read cursor was snapped to its target. A steady climb means the two clock rates differ — see below. |

All five are monotonic counters, not rates. A nonzero total is meaningless on
its own, because a startup transient contributes to it. Measure the **delta
over a known window**. Section 4 gives the arithmetic.

`recvResyncs` reads differently from the other four. The upstream read
position is a free-running cursor: it advances every cycle and is snapped
back to its target only when it leaves the safe window **and** the HAL's
write head moved that cycle (the head-moved gate — see section 3.4). An
occasional snap absorbs a scheduling hiccup and is harmless. A snap rate
that holds steady over minutes means the cursor is masking a clock-rate
error as periodic corrections — read `recvResyncs` first whenever the
other four move, because a snap explains them.

**Session-shape caveat.** A session that uses only one direction of the
device never advances the other direction's head (a playback-only client
never runs ReadInput; a recording-only client never runs WriteMix). The
snap rules are gated on the head moving, and the counters only measure
the cursor's own walk — so in a one-direction session the *idle*
direction's counters read flat, which is correct but proves nothing about
that direction. Do not read a flat idle-direction counter as "healthy
capture path"; it is flat because nothing is happening there. The
heartbeat and the anchor (section 2.2) remain the liveness signals.

### 2.4 Is the latency what you asked for?

| Field | Source |
|-------|--------|
| `jitterFrames` | `JitterFrames` in `config.plist`; the HAL reports it as SafetyOffset |
| `netLatencyCycles` | `NetLatency` in `config.plist`; the netadapter's `-l` |
| `netRingFrames` | `NetRing` in `config.plist`; the netadapter's `-g` |
| `oneWayLatency` | derived from the three above and from P |

The pair `-l` and `-g` is coupled. `NetRing / 2` must exceed
`NetLatency * P`. A pair that breaks this rule xruns the **pi** while every
Mac-side field stays perfect. Use `just watch` for that case, because it shows
the pi's `xruns_1m` beside these fields.

---

## 3. The cadence counters

### 3.1 What they measure

The daemon positions both rings with free-running cursors
(`jackbridge/daemon/RingProjector.hpp`):

- The send position — where the daemon **writes** the pi's audio, for the
  HAL to read. This is the capture direction, pi to DAW. A free-running
  cursor (`mSendCursor`): it advances by P every cycle and is snapped to
  `send_target()` (one block plus cushion ahead of the HAL's read head)
  only when it leaves the safe window. This replaced the old delta-term
  `send_offset()`, which reconstructed the position from the last head
  sync and beat against the head's jumps. `sendResyncs` (on the 5 s
  `health` os_log line) counts those snaps.
- The recv position — where the daemon **reads** the DAW's audio, to send
  to the pi. This is the playback direction, DAW to pi. The same design:
  a free-running cursor (`mRecvCursor`) snapped to `recv_target()` (one
  settled block behind the HAL's write head) only when it leaves the safe
  window. `recvResyncs` counts those snaps.

The counters compare each cursor against the previous cycle's, as a
signed absolute-frame difference. The comparison answers the audible
question directly:

- The position **advanced by less than one period** (repeated or went
  backward). `ring_consume_stereo_interleaved` zeroes each slot as it
  copies it, so a repeat on the read side sends silence. This increments
  `dupReadCycles` / `dupWriteCycles`.
- The position **advanced by more than one period**. The frames in
  between were never copied. This increments `skipReadFrames` /
  `skipWriteFrames` by that count.

The send and recv pairs are the same two tests on the two cursors. A forward
snap advances farther than one period and its displacement lands in the skip
counter. A backward snap is recorded as one dup cycle; that counter records the
event, not its frame magnitude.

### 3.2 Why they count positions, not heads

Both positions advance on their own while a head stands still — the write
projection walks through a HAL stall, and a playback-only session never
advances the input head at all — so "the head did not move" says nothing
about whether the position repeated. Count positions.

### 3.3 Reading them together

The read pair tells you more together than apart.

| `dupReadCycles` | `skipReadFrames` | Meaning |
|-----------------|------------------|---------|
| flat | flat | Correct. The cursor walks the settled block contiguously. |
| climbs fast | flat | The cursor is not advancing. Every cycle reads the same slot. Check that the JACK cycle is running. |
| climbs slowly | climbs by P per dup | A snap is firing every few cycles. Read `recvResyncs`. |
| flat | climbs | The position jumped more than a period. Read `recvResyncs` and `healthDeltaMax`. |

Read `recvResyncs` before any of the four: a snap has a real audio cost and
explains a same-window movement in the others. Occasional snaps absorb
scheduling jitter; a steady snap rate means the two clock rates differ, and
that needs a different fix (see section 6).

### 3.4 The head-moved gate

Both snap rules fire only on cycles where their head published a new
position (`sendHeadMoved` / `recvHeadMoved`, captured in `check_progress`
before the head cache updates). A head that has not moved means no IO op
ran in that direction, so there is no live block to tear into and the
cursor's open-loop walk is correct. This is what keeps a one-direction
session healthy: before the gate, the frozen head made the error ramp look
like a fault, and the rules snapped against it in a steady rhythm — the
recv rule shipped that way in v12 and read as a phantom clock-rate error in
recording-only sessions (dupReadCycles climbing ~6666 over a 27 s window,
per the session-shape fuzz).

The gate does not weaken the rules on the cycles that matter: when the
head moves again, CoreAudio's sample time is host-clock-derived, so a
resumed head jumps by exactly the frames the clock advanced and the
walk's error is preserved — no snap, no cost. Only a head that genuinely
dropped frames (a device reset, not a pause) presents a real hazard at
resume, and the first moved-head cycle snaps it back.

---

## 4. Turning a counter into a duty cycle

A counter delta is only useful against a window and against P.

The daemon runs `f_s / P` cycles per second. At 48000 Hz and P = 64, that is
750 cycles per second.

    duty loss = dupReadCycles_per_second / (f_s / P)

Worked example, the period-mismatch bug, measured at N = 512 and P = 64:

    dupReadCycles climbed 656 per second
    750 cycles per second
    656 / 750 = 0.875

The daemon lost 7 of every 8 cycles. That matches N/P − 1 of every N/P
exactly, which is the signature of a missing walk. The pi received 64 real
frames and 448 zeroed frames out of every 512.

Use `skipReadFrames` for the same figure in frames:

    frames lost per second / f_s = fraction of audio never read

### How to take the measurement

Play a known signal. Take the counters, wait a known time, take them again.

```sh
(afplay tone.wav &) ; sleep 2
A=$(just shm); T0=$(date +%s); sleep 15; B=$(just shm); T1=$(date +%s)
```

Then subtract each field and divide by `T1 - T0`. Two rules:

- **Start playback first.** The daemon writes silence and returns early while
  `driverStatus` is not `STARTED`, so an idle stack reports every counter as
  flat and looks perfect.
- **Record N and P with the result.** A cadence figure without N/P cannot be
  compared against another run.

---

## 5. Symptom index

**Playback is almost silent, with a blip every so often.**
Read `halNFrames` against `jackPeriodFrames`, then `dupReadCycles`. A climb
at `(N/P − 1) × f_s / P` per second means every cycle inside one HAL cycle
read the same slot. If the climb is slower and pairs with `recvResyncs`,
the cursor is snapping periodically — see the rate-error entry below.

**Capture works but playback does not.**
The two directions use different positioning: the write side is recomputed
from the HAL's read head every cycle, the read side is a free-running
cursor. Compare `dupReadCycles` against `dupWriteCycles`, then read
`recvResyncs` to separate a snap from a lost cycle.

**The counters move but `recvResyncs` is flat.**
The cursor is free-running correctly and the daemon's own cycles missed:
the JACK cycle stalled or dropped. Read `healthDeltaMax` and the daemon's
xrun count.

**`recvResyncs` climbs at a steady rate.**
The cursor is being snapped at a fixed period, which means it drifts
against the target at a fixed rate: the two clock rates differ. The snaps
keep the audio correct but each costs a period. Watch it over ten minutes;
a steady rate is a rate error, and a rate error needs a different fix —
do not treat it as jitter.

**Everything is green and there is silence.**
Read `driverStatus` and `slavePortsConnected` first. Then check that no
component wrote another component's field. `jackbridge/shared/JackBridge.h`
holds the ownership table at the top of the file.

**The audio is noise, but every field looks healthy.**
Read `healthDeltaMax` and `healthSnaps`. A diverged timeline shows no other
symptom.

**The Mac is clean and the pi xruns.**
The `-l` and `-g` pair overruns. Run `just watch` and read the pi's
`xruns_1m` beside `netLatencyCycles` and `netRingFrames`.

**The pi is clean and the Mac xruns.**
The cushion is too small for the cable's worst-case round trip. Raise `-l`
and `-g` together.

**A rebuild seems not to have landed.**
Do not compare a binary's timestamp against a git commit date. You build
before you commit, so an older artifact is the normal case. Use
`just jack-verify`, which greps for the code itself.

---

## 6. Worked example: the period-mismatch bug

This example shows the whole method. It was found on 2026-08-30.

**Symptom.** The DAW used the device at N = 1024. The pi ran P = 64. Playback
was near silence, with a short blip about every 21 ms.

**Measurement.** A 440 Hz tone played through the device. `just shm` gave:

    jackPeriodFrames      64
    halNFrames            512
    dupReadCycles         climbing 656/s
    dupWriteCycles        climbing 750/s

656 / 750 is 7/8, which is `N/P − 1` of every `N/P` at N = 512. That number
named the cause before any code was read.

**Cause.** `recv_offset()` was a pure function of the HAL's write head. The
head moves once per HAL cycle. All 8 daemon cycles inside one HAL cycle
computed the same position, and the consume-side zeroing meant only the first
of them carried audio.

**Second finding.** `dupWriteCycles` at 750/s was a false alarm from the
counters themselves, not a second bug. Section 3.2 explains it.

**Fix.** `recv_offset()` gained the delta term that `send_offset()` already
had, clamped so the read stays inside the settled block. The counters moved
to ring positions.

**Result.** Measured over a 16 s window at N = 512 and P = 64:

    dupReadCycles    12      (was ~10500 in the same window)
    skipReadFrames   768     (12 events of 64 frames)
    dupWriteCycles   0       (was ~12000)

The loss fell from 87.5% of frames to 0.1%.

**Residual, closed in two stages.** The 12 remaining events were a beat:
eight JACK cycles and one HAL cycle do not divide evenly in time, so the
walk hit its clamp while the head had not moved, one cycle repeated, and
the next jumped two periods. The free-running read cursor removed the beat
on the playback side. The capture side later showed the same family of
defect through the delta term (`dupWriteCycles` climbing 2.2/s in the
clean regime): `send_offset()` reconstructed the write position from the
last head sync, and the reconstruction beat against the head's jumps. The
send cursor closed it — both directions now hold the same design, recorded
in `docs/plan-free-running-cursor.md`. Its successor risk is a clock-rate
mismatch, which surfaces as a steady `sendResyncs`/`recvResyncs` rate on
the 5 s health os_log line — the symptom index above covers it.

---

## 7. The pi side

The pi's helpers have no `os_log`. They write to stderr, and systemd routes
stderr to the journal. The unit is a system unit, so the journal needs `sudo`.

```sh
ssh pistomp@pistomp.local 'sudo journalctl -u pi-stomp-jackbridge.service -n 200'
```

Every helper prefixes its own name, so grep the prefix to isolate one stage:
`jackbridge-pi-up:`, `jackbridge-pi-down:`, `jackbridge-napi-rt:`,
`jackbridge-unpin-route:`, `jackbridge-xrun-watcher:`.

Two lines matter more than the rest. `jackbridge-pi-up` logs a **clamp
warning** when it rewrites a `NetLatency` or `NetRing` value, and a **wiring
warning** when a `jack_connect` fails. A failed edge is invisible in
`ports_wired`, because netadapter registers its ports whether or not anything
connects to them.

The pi holds no setting of its own. Do not hand-edit
`/etc/default/jackbridge`; the next start overwrites it. `CLAUDE.md`, section
"The pi's tuning lives on the Mac", holds the full key table.

---

## 8. Related documents

| Topic | File |
|-------|------|
| Which command to run, and why | `CLAUDE.md`, "Observe and recover" |
| Surprising behaviors, with citations | `docs/idiosyncrasies.md` |
| Latency math and tunables | `docs/LATENCY-MODEL.md` |
| The jitter and crackle investigation log | `docs/JITTER.md` |
| The plan for the last cadence defect | `docs/plan-free-running-cursor.md` |
| Architecture and clock domains | `docs/architecture.md` |
