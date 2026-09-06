# Architecture

## The two-process design

JackBridge is split across two long-running processes that share one POSIX shared-memory region:

```
┌────────────────────────────┐         ┌────────────────────────────┐
│  JackBridge daemon         │         │  coreaudiod                │
│  (userland CLI binary)     │         │   └─ JackBridge.driver     │
│                            │         │      (AudioServerPlugIn)   │
│  - JACK client             │   shm   │  - HAL device (2in/4out)   │
│  - jack_process_callback() │ ◄─────► │  - kAudioServerPlugIn_IO   │
│  - writes shm[in], reads   │ /Jack-  │  - reads shm[out], writes  │
│    shm[out]                │ Bridge  │    shm[in]                 │
└────────────┬───────────────┘         └─────────────┬──────────────┘
             │                                       │
        jackd JACK graph                       DAW / system audio
        (netJACK2 client →                     (selects "JackBridge"
         Pi over Ethernet)                      as CoreAudio device)
```

**Why two processes?** The HAL driver is loaded by `coreaudiod` and inherits its sandbox — it cannot link `libjack` cleanly (library validation, sandbox restrictions, ABI fragility). The daemon runs in normal user context with full access to libjack. Shared memory is the lowest-latency IPC primitive that works across that boundary.

**Why shm and not XPC/mach ports?** Audio is a steady-state high-bandwidth flow with no request/response semantics — RPC overhead is pure cost. Shm with proper atomics gives lock-free SPSC ring buffers and zero-copy publication. XPC would dominate the per-cycle cost at low buffer sizes.

## The clock-domain decision (load-bearing)

There are two ways to run jackd on the Mac alongside JackBridge. **Only one of them works without rate conversion inside JackBridge.**

### Config A: `jackd -d net` (network is the cycle driver)

```
Pi audio clock ─► netJACK2 ─► Mac jackd cycle ─► daemon ─► shm
                                                              │
Mac CoreAudio clock ─► HAL IO proc ─► driver ──────────────► shm
```

Mac jackd runs at the **Pi's crystal**. CoreAudio runs at the **Mac's crystal**. Two physical clocks → guaranteed drift → JackBridge would need asynchronous SRC with a PI controller on ring-buffer fill. ~10 days of work and a known-fragile control loop.

**Don't use this config.**

### Config B: `jackd -d coreaudio` + netJACK2 as an internal JACK client (chosen)

```
Mac CoreAudio clock ─► Mac jackd cycle ─► daemon ─► shm
                                                       │
Mac CoreAudio clock ─► HAL IO proc ─► driver ────────► shm

netJACK2 master client (inside jackd) ─► adaptive SRC ─► network ─► Pi
```

Mac jackd is driven by a CoreAudio device (the Mac's built-in output by default; overridable via `ClockDeviceUID` in `config.plist` — see `macos-setup.md`). netJACK2 runs as a JACK *client* in jackd and does its own adaptive resampling at the network boundary. That's the whole reason netJACK2 exists vs netJACK1 — it solved this problem in 2011 and has been in production ever since.

JackBridge sees both sides on the **same CoreAudio host clock**. No SRC, no PI controller. Ring buffers absorb buffer-size mismatch between JACK's period and CoreAudio's IO buffer, nothing more.

**Use this config.** It's also the canonical netJACK2 deployment pattern.

## What the daemon does

1. `jack_client_open("JackBridge")` — register with the Mac's jackd.
2. Register 4 input ports + 2 output ports (channel count is fixed, see `idiosyncrasies.md`).
3. Open and `mmap` the `/JackBridge` POSIX shm region.
4. In each `jack_process_callback`:
   - Copy JACK port input buffers → shm input rings.
   - Copy shm output ring → JACK port output buffers.
   - Stamp the heartbeat / host-time fields.
5. On `jack_on_shutdown`, mark shm dead and exit so launchd can restart it.

The daemon process is otherwise idle (`while(1) sleep(...)`). All real work is in the JACK realtime thread.

## What the driver does

1. `AudioServerPlugInDriverRef` boilerplate (forked from Apple's SimpleAudio sample — `SA_` prefix throughout).
2. Presents one `kAudioObjectClassID_AudioDevice` with fixed property tables: 1 input stream (2ch), 2 output streams (2ch each), 48 kHz, Float32, packed.
3. `mmap`s the `/JackBridge` shm region on `Initialize`.
4. In `BeginIOOperation` / `EndIOOperation` / `DoIOOperation`:
   - **Input (mic side):** read shm input ring → CoreAudio buffer (advances `readFrameIn`).
   - **Output (playback side):** CoreAudio buffer → shm output ring (advances `writeFrameOut`).
5. Reports timing via `GetZeroTimeStamp`, anchored to the daemon's stamped `zeroHostTime` when sync mode is engaged.

The driver runs inside `coreaudiod`. It must obey hardened-runtime sandbox rules and Apple's HAL IO realtime constraints: no allocation, no syscalls, no logging, no locks in the IO path.

## Shared memory layout

The shm region (`/JackBridge`) has a fixed control area followed by ring
buffers. The IPC contract lives once in `jackbridge/shared/JackBridge.h` and
is included by both the daemon and HAL driver. `JACKBRIDGE_PROTOCOL_VERSION`
guards incompatible layouts; both processes refuse to attach on mismatch.

The companion reads the control fields read-only for status display. The
daemon and driver remain the only writers.

The fixed layout contains the protocol/version and heartbeat fields, HAL
anchor timing fields, per-stream frame numbers, and the input/output ring
buffers. See the offset macros in `jackbridge/shared/JackBridge.h` for the
authoritative byte map.

Constants (`STRBUFNUM` and `STRBUFSZ`) provide the ring headroom at 48 kHz.
The chosen Config B topology keeps JackBridge out of the clock-domain SRC
path; netJACK2 performs adaptive resampling at the network boundary.

## Why no SRC, ever (in this fork)

Three reasons to keep SRC out of JackBridge:

1. **It's not needed under Config B.** Both sides share the CoreAudio host clock.
2. **It's already done correctly elsewhere.** netJACK2's adaptive resampling has been the canonical solution for network audio clock-domain crossing for 15 years.
3. **It would add latency, group delay, and a control-loop failure mode** for zero benefit in this topology.

If a future use case demands SRC (e.g. someone wants `jackd -d net` mode for some reason), it belongs in a separate fork or a build-time option, not in the default path.

## The free-running cursors and their snap windows

The daemon holds two absolute-frame cursors. Each advances by `nframes` once
per JACK cycle and is used at `cursor % ring_frames`. Both sides share one
clock, so the cursors free-run and only two hard hazards correct them.
`RingProjector` (`jackbridge/daemon/RingProjector.hpp`) holds the targets and
the window limits; `check_progress()` applies them.

Design history and the defect these replaced: `docs/plan-free-running-cursor.md`.

### The upstream (recv) window

`recv_target()` is `halOutputWriteHead - block - jitter`. The window comes from
the two ways a read can be wrong, not from the cushion:

| Edge | Limit | Why |
|------|-------|-----|
| Forward | `block + jitter - period` | Beyond it, this cycle's read of `[pos, pos+P)` touches the live block at the write head. Between head jumps the cursor legitimately runs up to `block - period` ahead of the target — that headroom is the walk the cursor replaced. A tighter limit snaps mid-walk and re-reads zeroed slots every few cycles. |
| Backward | `-block` | More than one settled block of extra latency. The data behind stays valid, so trailing costs latency and not correctness — but past one block it is staler than the alignment we advertise. |

With `block = max(N, P)` the cursor's sawtooth against the head's jump fits
inside this window for every `N`, `P` and every seed phase. The forward edge is
the bound by construction, and the backward edge holds because
`block >= (N+P)/2`.

### The downstream (send) window

`send_target()` is `halInputReadHead + block + jitter`, the mirror.

| Edge | Limit | Why |
|------|-------|-----|
| Backward (torn) | `err < N - block - jitter` | This cycle's write of `[pos, pos+P)` lands inside the consumer's live block `[H, H+N)`. Torn audio. `N` here is the true HAL block (`hal_block_frames`), not the clearance. |
| Forward (lap) | `err > ring - block - jitter - P` | The write wraps a full ring onto the consumer's current or next block and overwrites audio it has not consumed. |

There is deliberately no forward discipline edge on the send side. A stalled
consumer is absorbed by the cursor walking ahead open-loop, so the window is
wide and a snap means a real hazard rather than a hiccup. A re-anchor that
plants the cursor just before a head jump can cross the torn edge once — one
bounded snap, then healthy.

### Reading the counters

Every recv snap is published to `RECV_RESYNCS`, every send snap to
`SEND_RESYNCS`. A silent correction is how a clock-rate error hides: a steady
climb in either means the two rates differ.

The dup/skip counters are not suppressed on a snap cycle. A snap has a real
audio cost — silence re-read forward, frames skipped backward — and hiding it
would let a snap pass unmeasured. Read the resync counters first: if one moved,
it explains any same-window movement in the others.

### The gate

Each snap rule fires only on cycles where its head published a new position. A
head that does not move means the HAL ran no IO op in that direction, so there
is no live block to collide with and the open-loop walk is correct. Ungated,
the rules snap against frozen heads in a steady rhythm: the v12 recv rule
produced a phantom clock-rate error, about 6666 false snaps in a 27 s
recording-only window.
