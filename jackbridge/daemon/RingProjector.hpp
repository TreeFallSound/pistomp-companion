/*
 * RingProjector — daemon-side cursor projection onto the shared ring buffer.
 *
 * Lifted out of JackBridge::sendToCoreAudio / receiveFromCoreAudio so the
 * offset-arithmetic (currently "frameNum % ring_frames" open-loop) can be
 * unit-tested in isolation from the JACK process callback, the shm mapping,
 * and the rest of the daemon's lifecycle.
 *
 * The bug under test (investigation-bug1.md): this struct currently has no
 * access to the HAL's published read/write heads, and produces the same
 * offset it would produce in a one-sided system — i.e. zero safety lead.
 * The fix (next commit) gives it the HAL heads and the configured
 * JitterFrames margin; the seam is what makes that testable.
 *
 * The two methods below are *byte-for-byte* equivalent to the inline
 * expressions in JackBridge.cpp @ the time of extraction. Until the
 * projection logic changes, a default-constructed RingProjector with
 * frame_cursor = FrameNumber reproduces the existing daemon exactly.
 *
 * Pure functions, no shm, no atomics, no globals — see tests/.
 */
#pragma once
#include <cstdint>

struct RingProjector {
    uint32_t ring_frames;   // == STRBUFNUM/2 == 4096 today; compile-time constant on the wire
    uint64_t frame_cursor;  // open-loop frame counter, advanced by exactly nframes per cycle

    // Where the daemon writes its next nframes chunk. Today: frame_cursor % ring_frames.
    // After the fix: ((halReadHead + jitter) + (frame_cursor - lastSyncedFrame)) % ring_frames,
    // using the HAL's published read head plus the configured safety margin.
    uint32_t send_offset() const {
        return static_cast<uint32_t>(frame_cursor % ring_frames);
    }

    // Where the daemon reads its last-written chunk from. Today:
    // (frame_cursor - nframes) % ring_frames. The unsigned underflow when
    // frame_cursor < nframes is intentional — preserved bug-for-bug from
    // JackBridge::receiveFromCoreAudio until the startup path is fixed.
    uint32_t recv_offset(int nframes) const {
        return static_cast<uint32_t>((frame_cursor - static_cast<uint64_t>(nframes)) % ring_frames);
    }
};
