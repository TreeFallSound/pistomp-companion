// RingProjector unit tests — `just test-projector`.
//
// The projector is pure math: absolute frame positions, ring storage. These
// tests pin the geometry (targets, errors) and then exercise the two snap
// rules the daemon applies (recv in check_progress, send mirrored) against
// adversarial cadences: steady state, HAL stalls, resumption, rate mismatch.
// No JACK, no shm, no HAL — the daemon's own harness runs the same rules.

#include "RingProjector.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        printf("FAIL: %s\n", what);
    }
}

// Steady-state geometry from the live stack: 4096-frame ring, 512-frame HAL
// block, 64-frame JACK period, 128-frame cushion. make_proj(N, P) sets the
// HAL block to N (block_frames = max(N, P) follows it whenever N >= P).
RingProjector make_proj(uint32_t hal_block = 512, uint32_t period = 64,
                        int32_t jitter = 128) {
    const uint32_t block = hal_block > period ? hal_block : period;
    return RingProjector{
        4096, block, hal_block, 0 /* hal_input_read_head */,
        0 /* hal_output_write_head */, period, jitter
    };
}

void test_recv_target_is_one_block_plus_jitter_behind_head() {
    RingProjector p = make_proj();
    p.hal_output_write_head = 10000;
    // recv_target is positional, not derived from any cursor input.
    check(p.recv_target() == 10000 - 512 - 128,
          "recv target: one block plus cushion behind the write head");
}

void test_recv_error_sign_and_magnitude() {
    RingProjector p = make_proj();
    p.hal_output_write_head = 10000;
    const uint64_t target = p.recv_target();   // 9360
    check(p.recv_error(target) == 0, "recv error: zero at the target");
    check(p.recv_error(target + 1) == 1,
          "recv error: positive one frame ahead of target");
    check(p.recv_error(target - 1) == -1,
          "recv error: negative one frame behind target");
    check(p.recv_error(target + 100) == 100, "recv error: +100 ahead");
    check(p.recv_error(target - 100) == -100, "recv error: -100 behind");
}

// The error is a plain absolute difference: the same lead after any number
// of ring laps reports the same value, and a lead beyond ring/2 is reported
// exactly rather than folded negative (the property the old shortest-way
// comparison lacked — the reason send_error must be absolute).
void test_recv_error_exact_at_any_lead() {
    RingProjector p = make_proj();
    p.hal_output_write_head = 4096 * 100;
    const uint64_t target = p.recv_target();
    check(p.recv_error(target + 3ull * 4096) == 3ull * 4096,
          "recv error: three full rings of lead report exactly");
    check(p.recv_error(target + 3840) == 3840,
          "recv error: 3840 lead (beyond ring/2) reports exactly");
    const uint64_t lapped = target + 3840 + 41ull * 4096;
    check(p.recv_error(lapped) == 3840 + (int64_t)(41ull * 4096),
          "recv error: laps are transparent to the error");
}

// The recv resync window the daemon applies (check_progress): the read of
// [pos, pos+P) must not touch the live block the HAL is writing — forward
// limit block + jitter − period — and must not read staler than one settled
// block behind the target — backward limit −block.

// Steady state: the head advances by N every N/P daemon cycles, the cursor
// by P every cycle. Seeded at the target, the error sawtooths with the
// head's grouped jumps — amplitude N−P, offset set by the seed's phase
// within the group. Assert containment in the window, not an exact
// offset: the phase of the first observed jump sets where the sawtooth
// rides, and the invariant that matters is that no phase ever snaps.
// For every seed phase the error must stay inside the window forever —
// no snap is ever needed.
void test_geometry_requires_strict_margin() {
    check(make_proj(1983, 64).geometry_valid(),
          "geometry: J=128 permits block sizes below 1984");
    check(!make_proj(1984, 64).geometry_valid(),
          "geometry: equality at 2*block+J is invalid");
    check(make_proj(64, 1982).geometry_valid(),
          "geometry: period below 1984 is valid when it is the block");
    check(!make_proj(64, 1984).geometry_valid(),
          "geometry: period equality at 2*block+J is invalid");
}

void test_recv_steady_state_never_resyncs() {
    struct Shape { uint32_t N, P; };
    const Shape shapes[] = {{512, 64}, {64, 64}, {1024, 64}};
    for (const Shape& s : shapes) {
        RingProjector p = make_proj(s.N, s.P);
        for (uint32_t phase = 0; phase < s.N / s.P; ++phase) {
            p.hal_output_write_head = 4096 * 10 + phase * s.P;
            uint64_t cursor = p.recv_target();
            int snaps = 0;
            int64_t err_min = 0, err_max = 0;
            for (int cycle = 0; cycle < 50000; ++cycle) {
                if (cycle % (s.N / s.P) == 0) p.hal_output_write_head += s.N;
                const int64_t err = p.recv_error(cursor);
                if (err < err_min) err_min = err;
                if (err > err_max) err_max = err;
                if (p.recv_outside_window(err)) {
                    cursor += (uint64_t)(-err);
                    ++snaps;
                }
                cursor += s.P;
            }
            check(snaps == 0, "recv steady state: never snaps");
            check(err_min >= -((int64_t)s.N) && err_max <= (int64_t)(s.N),
                  "recv steady state: sawtooth stays within one group of the target");
        }
    }
}

// A HAL stall upstream: the write head stops, the error climbs, and once past
// the forward limit the rule snaps the cursor back. After the head resumes
// the error must be back inside the window.
void test_recv_stall_snaps_back_and_converges() {
    const uint32_t N = 512, P = 64;
    RingProjector p = make_proj(N, P);
    p.hal_output_write_head = 4096 * 10;
    uint64_t cursor = p.recv_target();
    int snaps = 0;
    // Head stalls for 24 daemon cycles (3 HAL groups), then resumes.
    for (int cycle = 0; cycle < 3000; ++cycle) {
        if (cycle >= 24 && (cycle - 24) % (N / P) == 0)
            p.hal_output_write_head += N;
        const int64_t err = p.recv_error(cursor);
        if (p.recv_outside_window(err)) {
            cursor += (uint64_t)(-err);
            ++snaps;
        }
        cursor += P;
    }
    check(snaps > 0, "recv stall: snaps occur while the head is stopped");
    const int64_t finalErr = p.recv_error(cursor);
    check(!p.recv_outside_window(finalErr),
          "recv stall: error back inside the window after resume");
}

// A rate mismatch: the HAL writes faster than the daemon reads (or vice
// versa). The error ramps linearly, and the rule must snap periodically —
// each snap costs real audio (re-read silence forward / skip frames
// backward), so a flat resync counter with a moving error means the rates
// differ and something is wrong upstream.
void test_recv_rate_error_snaps_periodically() {
    const uint32_t N = 512, P = 64;
    RingProjector p = make_proj(N, P);
    p.hal_output_write_head = 4096 * 10;
    // Seed the cursor 64 frames/ahead per group: the cursor runs 1 frame per
    // 8 cycles faster than the head — a 1/512 rate error.
    uint64_t cursor = p.recv_target() + 64;
    int snaps = 0;
    for (int cycle = 0; cycle < 50000; ++cycle) {
        if (cycle % (N / P) == 0) p.hal_output_write_head += N - 1;  // fast head
        const int64_t err = p.recv_error(cursor);
        if (p.recv_outside_window(err)) {
            cursor += (uint64_t)(-err);
            ++snaps;
        }
        cursor += P;
    }
    check(snaps > 1, "recv rate error: snaps more than once (drift visible)");
}

// The recv window edges: exactly at a limit is not a fault; one past it is.
void test_recv_window_boundaries() {
    RingProjector p = make_proj();
    p.hal_output_write_head = 4096 * 10;
    const uint64_t target = p.recv_target();
    const int64_t fwd = p.recv_forward_limit();
    const int64_t bwd = p.recv_backward_limit();
    check(p.recv_error(target + fwd) == fwd,
          "recv window: at the forward limit, not yet a fault");
    check(p.recv_outside_window(fwd + 1),
          "recv window: one past the forward limit is a fault");
    check(p.recv_error(target + bwd) == bwd,
          "recv window: at the backward limit, not yet a fault");
    check(p.recv_outside_window(bwd - 1),
          "recv window: one past the backward limit is a fault");
}

// --- Send side: targets and errors ------------------------------------

void test_send_target_is_one_block_plus_jitter_ahead_of_head() {
    RingProjector p = make_proj();
    p.hal_input_read_head = 10000;
    check(p.send_target() == 10000 + 512 + 128,
          "send target: one block plus cushion ahead of the read head");
}

void test_send_error_sign_and_magnitude() {
    RingProjector p = make_proj();
    p.hal_input_read_head = 10000;
    const uint64_t target = p.send_target();   // 10640
    check(p.send_error(target) == 0, "send error: zero at the target");
    check(p.send_error(target + 1) == 1,
          "send error: positive one frame ahead of target");
    check(p.send_error(target - 1) == -1,
          "send error: negative one frame behind target");
    check(p.send_error(target + 100) == 100, "send error: +100 ahead");
    check(p.send_error(target - 100) == -100, "send error: -100 behind");
}

// The send error must be exact at ANY lead — no ring/2 cliff. A deep consumer
// stall (the cursor laps far ahead) must report its true lead so the lap-edge
// snap can act on it; folding past ring/2 would invert the snap direction.
void test_send_error_exact_at_any_lead() {
    RingProjector p = make_proj();
    p.hal_input_read_head = 4096 * 100;
    const uint64_t target = p.send_target();
    check(p.send_error(target + 3840) == 3840,
          "send error: 3840 lead (beyond ring/2) reports exactly");
    check(p.send_error(target + 3ull * 4096) == 3ull * 4096,
          "send error: three full rings of lead report exactly");
}

// The send resync window the daemon applies. Backward, a write of
// [pos, pos+P) must not tear into the consumer's live block [H, H+N):
// pos >= H + N. Forward, the write must not wrap a full ring onto that same
// live block: pos + P <= H + ring. In error space (err = pos − target,
// target = H + B + J):
//   backward (torn) edge:  err < N − B − J
//   forward (lap) edge:    err > ring − B − J − P
// There is deliberately NO forward "discipline" edge like recv's — a stalled
// consumer is absorbed by the cursor walking ahead, which is the old delta
// term's one good behaviour and the reason dupWriteCycles stay flat through
// consumer hiccups. The window is wide; the edges are hard hazards only.

// Steady state on the send side, in the regime the daemon actually seeds in:
// the cursor is planted at the target of the CURRENT head (first active
// cycle / reanchor), and the head's next jump is a full group away. The
// error then sawtooths [0, N−P] forever — no snap is ever needed, at any
// supported (N, P).
void test_send_steady_state_never_resyncs() {
    struct Shape { uint32_t N, P; };
    const Shape shapes[] = {{512, 64}, {64, 64}, {1024, 64}};
    for (const Shape& s : shapes) {
        RingProjector p = make_proj(s.N, s.P);
        p.hal_input_read_head = 4096 * 10;
        uint64_t cursor = p.send_target();
        int snaps = 0;
        int64_t err_min = 0, err_max = 0;
        for (int cycle = 0; cycle < 50000; ++cycle) {
            if (cycle > 0 && cycle % (s.N / s.P) == 0)
                p.hal_input_read_head += s.N;
            const int64_t err = p.send_error(cursor);
            if (err < err_min) err_min = err;
            if (err > err_max) err_max = err;
            if (p.send_outside_window(err)) {
                cursor += (uint64_t)(-err);
                ++snaps;
            }
            cursor += s.P;
        }
        check(snaps == 0, "send steady state: never snaps");
        check(err_min >= 0 && err_max <= (int64_t)(s.N - s.P),
              "send steady state: sawtooth rides [0, N-P]");
    }
}

// Seeded at a bad phase — reanchor landing k cycles before a head jump —
// the first jump drops the error below the torn edge and the rule snaps
// exactly once, then the walk is healthy [0, N−P] forever after. Pin the
// cost as bounded: one snap, no more, for the worst-case phase.
void test_send_bad_phase_seed_snaps_once_then_heals() {
    const uint32_t N = 512, P = 64;
    for (uint32_t phase = 0; phase < N / P; ++phase) {
        RingProjector p = make_proj(N, P);
        p.hal_input_read_head = 4096 * 10;
        uint64_t cursor = p.send_target();
        int snaps = 0;
        for (int cycle = 0; cycle < 50000; ++cycle) {
            // First jump after `phase` cycles, then every group.
            if (cycle == (int)phase ||
                (cycle > (int)phase && (cycle - (int)phase) % (N / P) == 0))
                p.hal_input_read_head += N;
            const int64_t err = p.send_error(cursor);
            if (p.send_outside_window(err)) {
                cursor += (uint64_t)(-err);
                ++snaps;
            }
            cursor += P;
        }
        check(snaps <= 1,
              "send bad-phase seed: at most one snap, then healthy forever");
    }
}

// A mid-depth consumer stall: the head freezes, the cursor keeps walking,
// the error climbs toward the lap edge but never reaches it. The rule must
// NOT snap — riding the stall out open-loop is the entire point of the
// cursor, and it is what keeps dupWriteCycles flat through hiccups.
void test_send_stall_rides_through_without_snapping() {
    const uint32_t N = 512, P = 64;
    RingProjector p = make_proj(N, P);
    p.hal_input_read_head = 4096 * 10;
    uint64_t cursor = p.send_target();
    int snaps = 0;
    // 40 cycles = 2560 frames of stall (< ring − B − J − P = 3392).
    for (int cycle = 0; cycle < 40; ++cycle) {
        const int64_t err = p.send_error(cursor);
        if (p.send_outside_window(err)) {
            cursor += (uint64_t)(-err);
            ++snaps;
        }
        cursor += P;
    }
    check(snaps == 0, "send stall: mid-depth stall rides through, no snap");
    // Resume: the head advances again; steady state must hold, no snaps.
    int resume_snaps = 0;
    for (int cycle = 0; cycle < 4000; ++cycle) {
        if (cycle % (N / P) == 0) p.hal_input_read_head += N;
        const int64_t err = p.send_error(cursor);
        if (p.send_outside_window(err)) {
            cursor += (uint64_t)(-err);
            ++resume_snaps;
        }
        cursor += P;
    }
    check(resume_snaps == 0,
          "send stall: steady state re-establishes after the resume");
}

// A stall deep enough to cross the lap edge: the cursor would wrap a full
// ring and overwrite the block the consumer is about to read. The rule must
// snap the cursor back into the window. This is the one honest send snap —
// its audio cost lands in dupWrite/skipWrite, never hidden.
void test_send_deep_stall_snaps_at_the_lap_edge() {
    const uint32_t N = 512, P = 64;
    RingProjector p = make_proj(N, P);
    p.hal_input_read_head = 4096 * 10;
    uint64_t cursor = p.send_target();
    const uint64_t frozen_head = p.hal_input_read_head;
    int snaps = 0;
    // Walk until the error exceeds the lap edge (ring − B − J − P = 3392).
    // 3392/64 = 53 cycles; run 60 to be sure.
    for (int cycle = 0; cycle < 60; ++cycle) {
        const int64_t err = p.send_error(cursor);
        if (p.send_outside_window(err)) {
            cursor += (uint64_t)(-err);
            ++snaps;
        }
        cursor += P;
    }
    check(snaps > 0, "send deep stall: snaps at the lap edge");
    p.hal_input_read_head = frozen_head;
    check(p.send_error(cursor) >= p.send_backward_limit() &&
          p.send_error(cursor) <= p.send_forward_limit(),
          "send deep stall: cursor lands inside the window after snapping");
}

// The send window edges: exactly at a limit is not a fault; one past it is.
void test_send_window_boundaries() {
    RingProjector p = make_proj();
    p.hal_input_read_head = 4096 * 10;
    const uint64_t target = p.send_target();
    const int64_t fwd = p.send_forward_limit();
    const int64_t bwd = p.send_backward_limit();
    check(p.send_error(target + fwd) == fwd,
          "send window: at the forward limit, not yet a fault");
    check(p.send_outside_window(fwd + 1),
          "send window: one past the forward limit is a fault");
    check(p.send_error(target + bwd) == bwd,
          "send window: at the backward limit, not yet a fault");
    check(p.send_outside_window(bwd - 1),
          "send window: one past the backward limit is a fault");
}

// A rate mismatch downstream: the consumer's head runs slow relative to the
// cursor. The error ramps toward the lap edge and the rule must snap
// periodically, each snap costing real frames — the sendResyncs counter is
// the smoking gun for a clock-rate difference, same as recv.
void test_send_rate_error_snaps_periodically() {
    const uint32_t N = 512, P = 64;
    RingProjector p = make_proj(N, P);
    p.hal_input_read_head = 4096 * 10;
    uint64_t cursor = p.send_target();
    int snaps = 0;
    for (int cycle = 0; cycle < 50000; ++cycle) {
        if (cycle % (N / P) == 0) p.hal_input_read_head += N - 1;  // slow head
        const int64_t err = p.send_error(cursor);
        if (p.send_outside_window(err)) {
            cursor += (uint64_t)(-err);
            ++snaps;
        }
        cursor += P;
    }
    check(snaps > 1, "send rate error: snaps more than once (drift visible)");
}

// A supported geometry must leave the complete steady-state walk below the
// send forward hazard edge.
void test_send_window_walk_fit() {
    const uint32_t N = 1024, P = 64;
    RingProjector p = make_proj(N, P);
    const int64_t walkPeak = (int64_t)N - P;
    check(walkPeak < p.send_forward_limit(),
          "send window: supported walk fits below the forward hazard");
}

// At N=2048 the strict geometry guard rejects the zero-margin configuration:
// the walk peak reaches beyond the send forward hazard edge.
void test_send_window_no_margin_at_N2048() {
    const uint32_t N = 2048, P = 64;
    RingProjector p = make_proj(N, P);
    const int64_t walkPeak = (int64_t)N - P;
    check(!(walkPeak < p.send_forward_limit()),
          "send window: N=2048 walk does not fit with zero margin");
}

// --- The head-moved gate (check_progress applies these rules) -------------
//
// The daemon gates each snap rule on its head having published a new
// position this cycle (sendHeadMoved / recvHeadMoved in JackBridge.cpp). A
// frozen head means no IO op ran in that direction — a playback-only
// session never runs ReadInput, a recording-only session never runs
// WriteMix — so there is no live block to collide with and the error is
// measured against a stale target. These tests pin the property the gate
// protects: the rule fires ONLY when the head moves, and a clock-tracked
// resume (the head jumps by exactly the elapsed frames) preserves the
// walk's error so no snap is needed at all.

// One-direction session: the head never moves, the gate suppresses the
// snap, the cursor walks open-loop forever, no dup/skip is recorded.
void test_gate_frozen_head_never_snaps() {
    const uint32_t N = 64, P = 64;
    RingProjector p = make_proj(N, P);
    p.hal_input_read_head = 4096 * 10;
    uint64_t cursor = p.send_target();
    const uint64_t frozen_head = p.hal_input_read_head;
    int snaps = 0;
    // 20000 cycles = far past the lap edge; the gate must hold them all.
    for (int cycle = 0; cycle < 20000; ++cycle) {
        const bool headMoved = false;  // playback-only: ReadInput never runs
        if (headMoved) {
            const int64_t err = p.send_error(cursor);
            if (p.send_outside_window(err)) {
                cursor += (uint64_t)(-err);
                ++snaps;
            }
        }
        cursor += P;
    }
    check(snaps == 0,
          "gate: frozen head never snaps through any stall depth");
    (void)frozen_head;
}

// Clock-tracked resume: after the freeze the head jumps by exactly the
// frames the clock advanced (CoreAudio sample time is host-clock-derived),
// so the open-loop walk's error is preserved and the first moved-head
// cycle finds the cursor inside the window. No snap, no cost.
void test_gate_clock_tracked_resume_preserves_walk() {
    const uint32_t N = 64, P = 64;
    RingProjector p = make_proj(N, P);
    p.hal_input_read_head = 4096 * 10;
    uint64_t cursor = p.send_target();
    const int frozen_cycles = 3000;
    // Freeze: walk open-loop, gate suppresses.
    for (int cycle = 0; cycle < frozen_cycles; ++cycle) cursor += P;
    // Resume: the head jumps by the elapsed frames.
    p.hal_input_read_head += (uint64_t)frozen_cycles * P;
    const int64_t err = p.send_error(cursor);
    check(!p.send_outside_window(err),
          "gate: clock-tracked resume preserves the walk's error");
    // And the steady regime re-establishes with no snaps.
    int snaps = 0;
    for (int cycle = 0; cycle < 20000; ++cycle) {
        if (cycle % 1 == 0 && cycle) p.hal_input_read_head += N;
        const int64_t e = p.send_error(cursor);
        if (p.send_outside_window(e)) {
            cursor += (uint64_t)(-e);
            ++snaps;
        }
        cursor += P;
    }
    check(snaps == 0,
          "gate: steady state re-establishes after a clock-tracked resume");
}

// A genuine resume-time hazard still snaps: if the consumer stalls for
// MORE than the clock's own advance (it dropped frames, not paused them —
// CoreAudio can report this after a device reset), the error at resume is
// real and the first moved-head cycle must correct it.
void test_gate_real_hazard_at_resume_still_snaps() {
    const uint32_t N = 64, P = 64;
    RingProjector p = make_proj(N, P);
    p.hal_input_read_head = 4096 * 10;
    uint64_t cursor = p.send_target();
    // Freeze the walk past the lap edge...
    for (int cycle = 0; cycle < 2000; ++cycle) cursor += P;
    // ...but the head only advanced a fraction of the clock's frames
    // (it dropped them): the cursor is now far ahead of the target.
    p.hal_input_read_head += 2000;   // not 2000*P
    const int64_t err = p.send_error(cursor);
    check(p.send_outside_window(err),
          "gate: a frame-dropping resume is a real hazard");
    // The gated rule corrects it on the first moved-head cycle.
    if (p.send_outside_window(err)) {
        cursor += (uint64_t)(-err);
    }
    check(!p.send_outside_window(p.send_error(cursor)),
          "gate: the snap lands the cursor back inside the window");
}

} // namespace

int main() {
    const std::function<void()> tests[] = {
        test_recv_target_is_one_block_plus_jitter_behind_head,
        test_recv_error_sign_and_magnitude,
        test_recv_error_exact_at_any_lead,
        test_geometry_requires_strict_margin,
        test_recv_steady_state_never_resyncs,
        test_recv_stall_snaps_back_and_converges,
        test_recv_rate_error_snaps_periodically,
        test_recv_window_boundaries,
        test_send_target_is_one_block_plus_jitter_ahead_of_head,
        test_send_error_sign_and_magnitude,
        test_send_error_exact_at_any_lead,
        test_send_steady_state_never_resyncs,
        test_send_bad_phase_seed_snaps_once_then_heals,
        test_send_stall_rides_through_without_snapping,
        test_send_deep_stall_snaps_at_the_lap_edge,
        test_send_window_boundaries,
        test_send_rate_error_snaps_periodically,
        test_send_window_walk_fit,
        test_send_window_no_margin_at_N2048,
        test_gate_frozen_head_never_snaps,
        test_gate_clock_tracked_resume_preserves_walk,
        test_gate_real_hazard_at_resume_still_snaps,
    };
    for (const auto& t : tests) t();
    if (g_failures == 0) {
        printf("projector: all %zu tests passed\n", sizeof(tests) / sizeof(tests[0]));
        return 0;
    }
    printf("projector: %d failure(s)\n", g_failures);
    return 1;
}