/*
 * RingProjector — daemon-side cursor projection onto the shared ring buffer.
 *
 * Both the downstream ring (buf_down, daemon writes / HAL reads) and the
 * upstream ring (buf_up, HAL writes / daemon reads) are addressed here.
 *
 * Downstream — send_offset():
 *   Daemon writes at (halInputReadHead + jitter + delta) % ring_frames,
 *   where delta = frame_cursor − last_synced_frame is how far the daemon has
 *   advanced since the HAL's read head last moved. During a HAL stall the
 *   daemon keeps advancing through the ring sequentially (no overwrite), and
 *   when the HAL resumes it finds contiguous data starting at its new read
 *   position. jitter_frames must be ≥ maxBurst − (stall_JACK_cycles × P) to
 *   guarantee no starvation; JB_JITTER_FRAMES (320) covers the observed
 *   worst-case burst of 272 frames under conservative JACK/HAL stall
 *   correlation assumptions.
 *
 * Upstream — recv_offset():
 *   Daemon reads at (halOutputWriteHead − jitter) % ring_frames, trailing the
 *   HAL's write head by jitter_frames. During a HAL stall (HAL doesn't write)
 *   the daemon reads the same position each cycle (repeated frames), which is
 *   less audible than reading ahead into unwritten territory.
 *
 * ring_frames MUST be a power of two (currently 4096 = STRBUFNUM/2).
 * Unsigned wrap-around on uint64_t arithmetic is intentional for recv_offset:
 * (A − B) % 2^k == (A − B + 2^64) % 2^k when 2^12 | 2^64, which it does.
 *
 * Pure functions, no shm, no atomics, no globals — testable in isolation.
 */
#pragma once
#include <cstdint>

struct RingProjector {
    uint32_t ring_frames;          // STRBUFNUM/2 = 4096; must be power of two
    uint64_t frame_cursor;         // daemon's FrameNumber (advances by nframes/cycle)
    uint64_t hal_input_read_head;  // HAL mInputTime.mSampleTime (downstream consumer)
    uint64_t hal_output_write_head;// HAL mOutputTime.mSampleTime (upstream producer)
    uint64_t last_synced_frame;    // frame_cursor when hal_input_read_head last changed
    int32_t  jitter_frames;        // JB_JITTER_FRAMES = 320

    // Downstream write position: JF frames ahead of HAL read head, plus the
    // daemon's open-loop delta since the last HAL advance. Handles stalls.
    uint32_t send_offset() const {
        const uint64_t delta = frame_cursor - last_synced_frame;
        return static_cast<uint32_t>(
            (hal_input_read_head + static_cast<uint64_t>(jitter_frames) + delta)
            % ring_frames);
    }

    // Upstream read position: JF frames behind HAL write head. Unsigned
    // underflow is safe because ring_frames is a power of two.
    uint32_t recv_offset() const {
        return static_cast<uint32_t>(
            (hal_output_write_head - static_cast<uint64_t>(jitter_frames))
            % ring_frames);
    }
};
