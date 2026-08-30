/*
 * RingCopy — copy/clear into and out of a single stereo-float ring buffer
 * slot, parameterised by offset and frame count.
 *
 * Lifted out of JackBridge::sendToCoreAudio and receiveFromCoreAudio so the
 * inner loop can be unit-tested without JACK or shm.
 *
 * Ring wrap is handled via a power-of-2 bitmask. ring_frames MUST be a
 * power of two — the static_assert in JackBridge.h guarantees this for the
 * current ring layout (STRBUFNUM/2 = 4096 = 2^12).
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
// `in` is laid out as `in[2][nframes]` (left, right). ring_frames must be a
// power of two.
static inline void ring_write_stereo_interleaved(
        sample_t* ring,
        uint32_t ring_frames,
        uint32_t offset,
        const sample_t* in_l,
        const sample_t* in_r,
        int nframes) {
    const uint32_t mask = ring_frames - 1;  // power-of-2 wrap
    for (int i = 0; i < nframes; i++) {
        const uint32_t pos = (offset + (uint32_t)i) & mask;
        ring[pos * 2 + 0] = in_l[i];
        ring[pos * 2 + 1] = in_r[i];
    }
}

// Read nframes interleaved stereo samples from `ring` at `offset` into
// `out_l` / `out_r`. Does NOT clear the slot. ring_frames must be a power
// of two.
static inline void ring_read_stereo_interleaved(
        const sample_t* ring,
        uint32_t ring_frames,
        uint32_t offset,
        sample_t* out_l,
        sample_t* out_r,
        int nframes) {
    const uint32_t mask = ring_frames - 1;
    for (int i = 0; i < nframes; i++) {
        const uint32_t pos = (offset + (uint32_t)i) & mask;
        out_l[i] = ring[pos * 2 + 0];
        out_r[i] = ring[pos * 2 + 1];
    }
}

// Read nframes interleaved stereo samples from `ring` at `offset` into
// `out_l` / `out_r`, then zero the source slot. ring_frames must be a power
// of two.
static inline void ring_consume_stereo_interleaved(
        sample_t* ring,
        uint32_t ring_frames,
        uint32_t offset,
        sample_t* out_l,
        sample_t* out_r,
        int nframes) {
    const uint32_t mask = ring_frames - 1;
    for (int i = 0; i < nframes; i++) {
        const uint32_t pos = (offset + (uint32_t)i) & mask;
        out_l[i] = ring[pos * 2 + 0];
        out_r[i] = ring[pos * 2 + 1];
        ring[pos * 2 + 0] = 0.0f;
        ring[pos * 2 + 1] = 0.0f;
    }
}
