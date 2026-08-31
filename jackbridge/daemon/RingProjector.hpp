/*
 * RingProjector — daemon-side projection onto the shared ring buffers.
 *
 * Both the downstream ring (buf_down, daemon writes / HAL reads) and the
 * upstream ring (buf_up, HAL writes / daemon reads) are addressed here.
 *
 * Both heads are published in BeginIOOperation, BEFORE the HAL copies that
 * cycle's block. A head of H means the HAL is about to touch [H, H+block),
 * and the newest settled block is [H−block, H). Every position here is
 * therefore one block clear of the head, and jitter_frames is cushion on
 * top of that — nothing else.
 *
 * block_frames is max(HAL block, JACK period), because the two directions
 * need different clearances and the larger covers both: send_offset must
 * stay clear of the block the HAL is READING, which is the HAL's size, while
 * the upstream read must stay clear of the block the daemon itself reads,
 * which is the JACK period. The two are independent — a host sets the HAL's
 * size through kAudioDevicePropertyBufferFrameSize and nothing reconfigures
 * the ring when it does. jitter_frames therefore buys pure cushion: 0 means
 * the tightest correct alignment, not a hazard.
 *
 * Downstream — send_offset():
 *   The daemon writes at (halInputReadHead + block + jitter + delta)
 *   % ring_frames, where delta = frame_cursor − last_synced_read_frame is how
 *   far the daemon has advanced since the HAL's read head last moved. The
 *   block term keeps the write clear of the block the HAL is reading right
 *   now; during a HAL stall the daemon keeps advancing through the ring
 *   sequentially (no overwrite), and when the HAL resumes it finds contiguous
 *   data starting at its new read position.
 *
 * Upstream — the free-running cursor and recv_target():
 *   The daemon holds an absolute-frame read cursor (JackBridge's mRecvCursor)
 *   that advances by nframes once per JACK cycle and is read at
 *   `cursor & (ring_frames − 1)`.
 *
 *   recv_target() is the position that cursor should hold:
 *   (halOutputWriteHead − block − jitter) % ring_frames — one settled block
 *   plus cushion behind the write head. recv_error(cursor) reports the signed
 *   gap between cursor and target: positive means the cursor has run ahead
 *   toward the live block, negative means it trails behind into settled
 *   data.
 *
 *   A free-running cursor works because both sides run in one CoreAudio
 *   clock domain: jackd's coreaudio backend drives the daemon's cycle from
 *   the same crystal the HAL's IO proc runs on, so a cursor that advances
 *   by nframes per cycle stays in phase with the target indefinitely. It
 *   needs a correction only for jitter — a scheduling hiccup or a dropped
 *   cycle — never for drift.
 *
 *   The correction rule lives in JackBridge::check_progress(): snap the
 *   cursor to the target when recv_error leaves (−block_frames,
 *   block + jitter − period]. Each edge is physical. Forward: at
 *   block + jitter − period the read of [pos, pos+P) touches the live block
 *   at the write head; between head jumps the cursor legitimately runs up to
 *   block − period ahead (that headroom is the walk the cursor replaced), so
 *   a tighter edge would snap mid-walk. Backward: past one settled block the
 *   data behind is still valid but staler than the alignment we advertise.
 *   With block = max(N, P) that window contains the cursor's sawtooth
 *   against the head's jump for every N, P and every seed phase — a
 *   matched-rate free-run never snaps. A HAL stall needs no separate case:
 *   the head stops, the error grows negative past the backward edge, the
 *   rule snaps the cursor back, and the read parks on the last settled
 *   block.
 */
#pragma once
#include <cstdint>

struct RingProjector {
    uint32_t ring_frames;          // STRBUFNUM/2 = 4096; must be power of two
    uint32_t block_frames;         // clearance: max(HAL block, JACK period)
    uint64_t frame_cursor;         // daemon's FrameNumber (advances by nframes/cycle)
    uint64_t hal_input_read_head;  // HAL mInputTime.mSampleTime (downstream consumer)
    uint64_t hal_output_write_head;// HAL mOutputTime.mSampleTime (upstream producer)
    uint32_t period_frames;        // this cycle's JACK nframes (P)
    uint64_t last_synced_read_frame; // frame_cursor when hal_input_read_head last changed
    int32_t  jitter_frames;        // JB_JITTER_FRAMES, config.plist JitterFrames

    // Downstream write position: one block clear of the HAL's read head, plus
    // the cushion, plus the daemon's open-loop delta since the last HAL
    // advance. Handles stalls.
    uint32_t send_offset() const {
        const uint64_t delta = frame_cursor - last_synced_read_frame;
        return static_cast<uint32_t>(
            (hal_input_read_head + block_frames
             + static_cast<uint64_t>(jitter_frames) + delta)
            % ring_frames);
    }

    // Upstream target: the position the free-running cursor should hold —
    // one settled block plus cushion behind the HAL's write head. The daemon
    // reads at cursor & (ring_frames − 1), NOT here; recv_error reports the
    // gap between the two.
    uint32_t recv_target() const {
        return static_cast<uint32_t>(
            (hal_output_write_head - block_frames
             - static_cast<uint64_t>(jitter_frames))
            % ring_frames);
    }

    // How far the cursor is from the target, in ring frames, signed:
    //   > 0 : cursor runs ahead, toward the live block at the target's front
    //   < 0 : cursor trails behind, into older settled data
    // Exact while |error| < ring_frames / 2, which the resync window and any
    // bounded stall respect.
    int64_t recv_error(uint64_t cursor) const {
        const int64_t err =
            static_cast<int64_t>((cursor - recv_target()) % ring_frames);
        return err > (int64_t)(ring_frames / 2)
            ? err - (int64_t)ring_frames : err;
    }
};