/*
 * RingCopy — pure-function copy/clear into and out of a single stereo-float
 * ring buffer slot, parameterised by offset and frame count.
 *
 * Lifted out of JackBridge::sendToCoreAudio and receiveFromCoreAudio so the
 * wrap-not-handled inner loop can be unit-tested without JACK or shm.
 *
 * IMPORTANT: these functions do NOT handle wrap. If offset+nframes exceeds
 * ring_frames, the caller is responsible — that is the same behavior as
 * the inline code being replaced (see the "FIXME: should be consider buffer
 * overwrapping" comment in JackBridge.cpp). Wrap handling is a future
 * change and a future test.
 *
 * sample_t matches JackBridge.h's typedef (== float). NUM_STREAMS is the
 * per-stream iteration count the daemon uses (NUM_INPUT_STREAMS for write,
 * NUM_OUTPUT_STREAMS for read/consume).
 *
 * Pure functions, no shm, no atomics, no globals.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include "JackBridge.h"  // for sample_t

// Write nframes interleaved stereo samples from `in` into `ring` at `offset`.
// `in` is laid out as `in[2][nframes]` (left, right). Caller must guarantee
// offset + nframes <= ring_frames; wrap is intentionally unhandled.
static inline void ring_write_stereo_interleaved(
        sample_t* ring,
        uint32_t ring_frames,
        uint32_t offset,
        const sample_t* in_l,
        const sample_t* in_r,
        int nframes) {
    (void)ring_frames;  // reserved for future bounds check
    for (int i = 0; i < nframes; i++) {
        ring[(offset + i) * 2 + 0] = in_l[i];
        ring[(offset + i) * 2 + 1] = in_r[i];
    }
}

// Read nframes interleaved stereo samples from `ring` at `offset` into
// `out_l` / `out_r`. Does NOT clear the slot — see ring_consume_* for that.
// Caller must guarantee offset + nframes <= ring_frames.
static inline void ring_read_stereo_interleaved(
        const sample_t* ring,
        uint32_t ring_frames,
        uint32_t offset,
        sample_t* out_l,
        sample_t* out_r,
        int nframes) {
    (void)ring_frames;  // reserved for future bounds check
    for (int i = 0; i < nframes; i++) {
        out_l[i] = ring[(offset + i) * 2 + 0];
        out_r[i] = ring[(offset + i) * 2 + 1];
    }
}

// Read nframes interleaved stereo samples from `ring` at `offset` into
// `out_l` / `out_r`, then zero the source slot. Mirrors the consume pattern
// in JackBridge::receiveFromCoreAudio exactly: read, then clear, so the
// daemon can detect "HAL hasn't written here yet" by reading a zero.
static inline void ring_consume_stereo_interleaved(
        sample_t* ring,
        uint32_t ring_frames,
        uint32_t offset,
        sample_t* out_l,
        sample_t* out_r,
        int nframes) {
    (void)ring_frames;  // reserved for future bounds check
    for (int i = 0; i < nframes; i++) {
        out_l[i] = ring[(offset + i) * 2 + 0];
        out_r[i] = ring[(offset + i) * 2 + 1];
        ring[(offset + i) * 2 + 0] = 0.0f;
        ring[(offset + i) * 2 + 1] = 0.0f;
    }
}
