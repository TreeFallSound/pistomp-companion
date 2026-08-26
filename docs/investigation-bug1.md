# Investigation: Bug 1 — `JitterFrames` is cosmetic (daemon writes with zero safety lead)

Status: **verified**. The daemon and HAL use two independent modulo counters on the same ring buffer. There is no back-pressure, no projection, and no consumption of the `JitterFrames` knob.

---

## What the ring is

| Constant | Value | Evidence |
|----------|-------|----------|
| `STRBUFSZ` | `0x8000` = 32,768 bytes | `jackbridge/shared/JackBridge.h` |
| `AUDIO_SAMPLE_SIZE` | `sizeof(float)` = 4 bytes | `jackbridge/shared/JackBridge.h` |
| `STRBUFNUM` | `32768 / 4 = 8192` float slots | `jackbridge/shared/JackBridge.h` |
| `FramesPerBuffer` | `8192 / 2 = 4096` stereo frames | `jackbridge/daemon/JackBridge.cpp`, `jackbridge/driver/JackBridge/Plug-In/SA_Device.cpp` |

Each ring holds **4096 stereo float frames** (interleaved L/R). Both sides know this value; neither can change it at runtime.

---

## What the daemon does (data path)

`jackbridge/daemon/JackBridge.cpp:449`:

```cpp
int sendToCoreAudio(float** in, int nframes) {
    unsigned int offset = FrameNumber % FramesPerBuffer;
    // ... writes in[j*2][i] to buf_down[j]+(offset+i)*2 ...
}
```

`jackbridge/daemon/JackBridge.cpp:461`:

```cpp
int receiveFromCoreAudio(float** out, int nframes) {
    unsigned int offset = (FrameNumber - nframes) % FramesPerBuffer;
    // ... reads buf_up into out, then zeros buf_up ...
}
```

`FrameNumber` advances by exactly `nframes` per JACK cycle, regardless of what the HAL is doing. The modulo operator guards ring addressing.

The `g_jitter_frames` setting is not used in the daemon transfer functions.

---

## What the HAL does (data path)

`jackbridge/driver/JackBridge/Plug-In/SA_Device.cpp:1650`:

```cpp
void SA_Device::ReadInputData(int streamId, UInt32 inIOBufferFrameSize,
                               Float64 inSampleTime, void* outBuffer) {
    UInt64 theSampleTime = static_cast<UInt64>(inSampleTime);
    UInt32 theStartFrameOffset = theSampleTime % mRingBufferFrameSize;
    // ... memcpy from buf_down at offset ...
}
```

`jackbridge/driver/JackBridge/Plug-In/SA_Device.cpp:1690`:

```cpp
void SA_Device::WriteOutputData(int streamId, UInt32 inIOBufferFrameSize,
                                Float64 inSampleTime, const void* inBuffer) {
    UInt64 theSampleTime = static_cast<UInt64>(inSampleTime);
    UInt32 theStartFrameOffset = theSampleTime % mRingBufferFrameSize;
    // ... memcpy into buf_up at offset ...
}
```

The HAL's `inSampleTime` is computed in `GetZeroTimeStamp` from `shmZeroHostTime` plus elapsed wall-clock time. The HAL never reads the daemon's `FrameNumber`.

---

## What's missing: projection

Under Config B (same CoreAudio clock on both sides) the ring is supposed to act as a **single-producer / single-consumer FIFO** with a fixed safety margin. To do that, the daemon must position its write head relative to the HAL's *read* head, not relative to its own open-loop cycle count.

The HAL already publishes exactly what the daemon needs:

| shm field | HAL writer | Daemon reader | Evidence |
|-----------|------------|---------------|----------|
| `shmHalInputReadHead` | `BeginIOOperation` line 1600 | `check_progress` line 640 | `SA_Device.cpp:1597–1616` |
| `shmHalOutputWriteHead` | `BeginIOOperation` line 1604 | `check_progress` line 640 | `SA_Device.cpp:1597–1616` |

These values are published under a seqlock (bump odd, write, bump even) so the daemon can take a consistent snapshot. The daemon does take that snapshot, but **only to count `mSnapCount` and log it**:

```cpp
// daemon/JackBridge.cpp:640–660
do {
    s1 = shmHalAnchorSeq->load(...);
    halReadHead = shmHalInputReadHead->load(...);
    s2 = shmHalAnchorSeq->load(...);
} while ((s1 & 1) || s1 != s2);

if (halReadHead > 0 && isActive) {
    int64_t target = (int64_t)halReadHead + g_jitter_frames;
    int64_t diff = (int64_t)FrameNumber - target;
    if (diff < 0) diff = -diff;
    if (diff > kSnapThresholdFrames) {
        mSnapCount.fetch_add(1, std::memory_order_relaxed);
    }
}
```

`mSnapCount` is reset to zero every 5 seconds in `check_progress` and never read by anyone else.

---

## What `JitterFrames` was meant to do

Per `docs/LATENCY-MODEL.md`:

> **T_jf — HAL safety lead (`JitterFrames`)**> 1:1 — pure latency, no slip-ring effect (single clock domain)

The intent was:
- Daemon writes `JitterFrames` ahead of `shmHalInputReadHead`
- Daemon reads `JitterFrames` behind `shmHalOutputWriteHead`
- This safety margin absorbs thread-scheduling jitter on the Mac (daemon JACK callback vs HAL IOProc)

The code to do this does not exist. The field `g_jitter_frames` is loaded once at startup and then used only in the diagnostic block above.

---

## Why this causes clicks

Both sides advance by 64 frames per cycle *on average*, but:

1. CoreAudio can bunch multiple cycles into one IOProc call after a stall (jitter logs show `nframes=240`, `maxNFrames=316`).
2. The HAL's `sampleTime` is derived from wall-clock, so a 240-frame backlog means `sampleTime` jumps by 240 in one call.
3. The daemon only advanced by 64 in that same wall-clock window.
4. Because the daemon writes at `FrameNumber % 4096` and the HAL reads at `sampleTime % 4096`, a 176-frame gap opens between the write head and the read head.
5. When `sampleTime` finally catches up, the HAL reads from offsets the daemon already overwrote (ring wrap) or hasn't reached yet (empty read).

The jitter log from a live session (`docs/JITTER.md`) shows the signature:

```
jitter nframes=240 maxNFrames=240 cycles=3025 inLead{min=58 mean=63 max=287 nearMiss=0}
```

`maxNFrames=240` = CoreAudio delivered a 240-frame batch in one callback.
`min=58` = the HAL callback fired 58 frames after its nominal input time.
This is exactly what happens when the daemon and HAL are two independent modulo counters with no safety margin.

---

## Most pressing open questions

1. **What is the exact projection formula?**
   - Should `sendToCoreAudio` write at `(halReadHead + JitterFrames) % RING_FRAMES`?
   - Should `receiveFromCoreAudio` read at `(halWriteHead - JitterFrames) % RING_FRAMES`?
   - Or should the safety margin be split (half lead, half lag)?

2. **What happens during startup before the HAL has published any anchor?**
   - `shmHalInputReadHead` starts at 0, which would project the daemon to write at `offset = JitterFrames % 4096`.
   - Is that safe, or do we need a warm-up period where the daemon stays in open-loop mode?

3. **What happens on a wrap discontinuity?**
   - Both cursors are `uint64_t`, so wrapping to zero only happens after 2^64 frames (~8.5 million years), but the modulo result changes every time the cursor crosses a 4096 boundary.
   - Is `uint64_t` subtraction modulo-safe without explicit masking?

4. **Should projection replace the periodic anchor stamp, or coexist with it?**
   - The anchor stamp (`shmZeroHostTime` + `shmNumberTimeStamps`) currently fires every 4096 frames.
   - If the daemon now writes at a projected offset, does `GetZeroTimeStamp` still need the stamp, or can it derive time purely from `shmHalInputReadHead`?

5. **How do we handle a genuine xrun (netJACK2 packet loss) on the JACK side?**
   - If the daemon can't produce 64 frames because netJACK2 is late, `jack_process_callback` still fires (with zeros or silence).
   - Should the daemon advance `FrameNumber` anyway, or pause its cursor?
   - If it pauses, the projection window shrinks; if it advances, it may write stale/silent data into the ring.

6. **How big does `JitterFrames` actually need to be?**
   - Current default is 256 frames on a 4096-frame ring.
   - The jitter log shows worst-case scheduling spikes of ~240 frames.
   - Is 256 enough margin, or do we need to measure the actual scheduling distribution under load (e.g. during a REAPER session with plugins) to size it correctly?

7. **Should the fix be a hard snap or a gradual catch-up?**
   - A hard snap (`FrameNumber = halReadHead + JitterFrames`) is abrupt and may cause an audible discontinuity.
   - A gradual catch-up (e.g. advance by `nframes + 1` until aligned) is smoother but adds a control loop.
   - Since both sides are on the same clock (Config B), a snap should only happen after a stall/bunch event, not on every cycle.
