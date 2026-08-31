/*
 * RingProjector — daemon-side projection onto the shared ring buffers.
 *
 * Both the downstream ring (buf_down, daemon writes / HAL reads) and the
 * upstream ring (buf_up, HAL writes / daemon reads) are addressed here.
 *
 * Model (the same one ALSA and WASAPI use for device rings): every position
 * is an ABSOLUTE frame count from the stream's start — the daemon's
 * FrameNumber/cursors and the HAL's mSampleTime heads are all unbounded
 * uint64 counters in the same clock domain (syncMode is 1, so there is one
 * CoreAudio clock for both sides). The ring is just storage: the slot for
 * absolute frame f is f % ring_frames, and the consumer's live block is
 * [head, head+N) in absolute frames. Nothing ever wraps or folds, so a
 * signed difference between two absolute positions is exact at any stall
 * depth — there is no "shortest way around the ring" ambiguity to get
 * wrong, and no half-ring limit on how far ahead or behind a cursor may
 * legitimately be before the comparison itself goes bad.
 *
 * block_frames is max(HAL block N, JACK period P), because the two
 * directions need different clearances and the larger covers both: the
 * send write must stay clear of the block the HAL is READING, which is the
 * HAL's size N, while the upstream read must stay clear of the block the
 * daemon itself reads, which is P. The two are independent — a host sets
 * the HAL's size through kAudioDevicePropertyBufferFrameSize and nothing
 * reconfigures the ring when it does. jitter_frames is cushion on top of
 * the clearance — 0 means the tightest correct alignment, not a hazard.
 *
 * Upstream — the free-running read cursor and recv_target():
 *   The daemon holds an absolute-frame read cursor (JackBridge's mRecvCursor)
 *   that advances by nframes once per JACK cycle and is read at
 *   `cursor % ring_frames`.
 *
 *   recv_target() is the absolute position that cursor should hold:
 *   halOutputWriteHead − block − jitter — one settled block plus cushion
 *   behind the HAL's write head. recv_error(cursor) reports the signed gap
 *   between cursor and target: positive means the cursor has run ahead
 *   toward the live block the HAL is writing; negative means it trails
 *   behind into older settled data.
 *
 * Downstream — the free-running send cursor and send_target():
 *   The daemon holds an absolute-frame write cursor (JackBridge's
 *   mSendCursor) that advances by nframes once per JACK cycle and is
 *   written at `cursor % ring_frames`. This replaces the old delta-term
 *   send_offset(): instead of reconstructing the HAL's read position from
 *   the last head sync, the daemon accumulates the walk itself.
 *
 *   send_target() is the absolute position that cursor should hold:
 *   halInputReadHead + block + jitter — one block plus cushion ahead of
 *   the consumer's read head, the mirror of recv_target(). send_error()
 *   reports the signed gap: positive means the cursor has run ahead
 *   (toward the lap edge, a full ring past the head), negative means it
 *   trails toward the live read block at the head.
 *
 *   The same free-run argument holds as for the read cursor: one clock
 *   domain, matched rates, corrections only for jitter. A HAL stall needs
 *   no separate case — the head stops, the error runs positive, the cursor
 *   keeps walking ahead open-loop through the stall exactly as the old
 *   delta term did, and because the error is a plain absolute difference
 *   there is no ring/2 cliff for it to fall off. Only the two hard
 *   hazards snap: a cursor so far ahead it would wrap a full ring onto the
 *   consumer's live block, or one so far behind its write would tear into
 *   it. Snap-to-target on those, count every snap, and let the dup/skip
 *   counters record the audio cost.
 */

#pragma once
#include <cstdint>

struct RingProjector {
    uint32_t ring_frames;          // STRBUFNUM/2 = 4096; must be a power of two
    uint32_t block_frames;         // clearance: max(HAL block N, JACK period P)
    uint32_t hal_block_frames;     // consumer's live-block width: the true HAL N
    uint64_t hal_input_read_head;  // HAL mInputTime.mSampleTime (downstream consumer)
    uint64_t hal_output_write_head;// HAL mOutputTime.mSampleTime (upstream producer)
    uint32_t period_frames;        // this cycle's JACK nframes (P)
    int32_t  jitter_frames;        // JB_JITTER_FRAMES, config.plist JitterFrames

    bool geometry_valid() const {
        return static_cast<uint64_t>(ring_frames) >
               2ull * static_cast<uint64_t>(block_frames) +
               static_cast<uint64_t>(jitter_frames > 0 ? jitter_frames : 0);
    }

    int64_t recv_forward_limit() const {
        return static_cast<int64_t>(block_frames) + jitter_frames
             - static_cast<int64_t>(period_frames);
    }

    int64_t recv_backward_limit() const {
        return -static_cast<int64_t>(block_frames);
    }

    bool recv_outside_window(int64_t error) const {
        return error > recv_forward_limit() || error < recv_backward_limit();
    }

    int64_t send_backward_limit() const {
        return static_cast<int64_t>(hal_block_frames)
             - static_cast<int64_t>(block_frames) - jitter_frames;
    }

    int64_t send_forward_limit() const {
        return static_cast<int64_t>(ring_frames)
             - static_cast<int64_t>(block_frames) - jitter_frames
             - static_cast<int64_t>(period_frames);
    }

    bool send_outside_window(int64_t error) const {
        return error > send_forward_limit() || error < send_backward_limit();
    }
    // Downstream target: one block plus cushion AHEAD of the consumer's
    // read head — the mirror of recv_target(). Absolute, not folded mod the
    // ring. The daemon writes at cursor % ring_frames, NOT here;
    // send_error reports the gap between the two.
    uint64_t send_target() const {
        return hal_input_read_head + block_frames
             + static_cast<uint64_t>(jitter_frames);
    }

    // Upstream target: one settled block plus cushion BEHIND the HAL's
    // write head. Absolute, not folded mod the ring. The daemon reads at
    // cursor % ring_frames, NOT here; recv_error reports the gap.
    uint64_t recv_target() const {
        return hal_output_write_head - block_frames
             - static_cast<uint64_t>(jitter_frames);
    }

    // Signed gap from a target, in frames. Both operands are absolute
    // frame counts in the same unbounded clock domain, so the subtraction
    // is exact for any stall depth and any number of ring laps — no
    // shortest-way-around ambiguity, no half-ring limit.
    int64_t error_from(uint64_t cursor, uint64_t target) const {
        return static_cast<int64_t>(cursor - target);
    }

    // Send error: cursor's lead over the send target, signed.
    //   > 0 : cursor runs ahead, toward the lap edge (one full ring past
    //         the consumer's read head, where the write would land on data
    //         the consumer is about to read)
    //   < 0 : cursor trails behind, toward the consumer's live read block
    int64_t send_error(uint64_t cursor) const {
        return error_from(cursor, send_target());
    }

    // Recv error: cursor's lead over the recv target, signed.
    //   > 0 : cursor runs ahead, toward the live block at the target's front
    //   < 0 : cursor trails behind, into older settled data
    int64_t recv_error(uint64_t cursor) const {
        return error_from(cursor, recv_target());
    }
};