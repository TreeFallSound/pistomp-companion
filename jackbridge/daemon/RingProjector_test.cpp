// Unit test for RingProjector. The projector is pure — no shm, no JACK —
// so the cursor math is testable in isolation.
//
// Build and run via `just test-projector` (see the justfile).

#include "RingProjector.hpp"

#include <cstdio>
#include <cstdint>
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
// block, 64-frame JACK period, 128-frame cushion.
RingProjector make_proj(uint32_t block = 512, uint32_t period = 64,
                         int32_t jitter = 128) {
    return RingProjector{
        4096, block, 0 /* frame_cursor */, 0 /* hal_input_read_head */,
        0 /* hal_output_write_head */, period, 0 /* last_synced_read_frame */,
        jitter
    };
}

void test_target_is_one_block_plus_jitter_behind_head() {
    const RingProjector p = make_proj();
    // recv_target() is positional, not derived from any cursor input.
    const RingProjector q = make_proj();
    (void)q;
    const uint32_t target = p.recv_target();
    // W=0, block=512, jitter=128 -> (0 - 512 - 128) mod 4096 = 3456
    check(target == 3456, "target: one block plus cushion behind the head");
}

void test_error_sign_and_magnitude() {
    RingProjector p = make_proj();
    const uint32_t target = p.recv_target();   // 3456
    check(p.recv_error(target) == 0, "error: zero at the target");
    check(p.recv_error(target + 1) == 1,
          "error: positive one frame ahead of target");
    check(p.recv_error(target - 1) == -1,
          "error: negative one frame behind target");
    check(p.recv_error(target + 100) == 100, "error: +100 ahead");
    check(p.recv_error(target - 100) == -100, "error: -100 behind");
}

void test_error_wraps_the_ring_shortest_way() {
    RingProjector p = make_proj();
    const uint32_t target = p.recv_target();   // 3456
    // A cursor 100 frames behind across the wrap is -100, not +3996.
    const uint64_t behind_wrap = (target >= 100) ? target - 100 : target - 100 + 4096;
    check(p.recv_error(behind_wrap) == -100, "error: wraps shortest way backward");
    // 100 ahead across the wrap (3456+100=3556 < 4096, so no wrap) — use a
    // cursor 100 ahead via absolute frame arithmetic instead.
    check(p.recv_error(target + 100) == 100, "error: absolute frame arithmetic");
    // More than half a ring ahead must fold negative.
    check(p.recv_error(target + 3000) == 3000 - 4096,
          "error: beyond half a ring folds negative");
}

void test_error_is_exact_on_absolute_frames() {
    RingProjector p = make_proj();
    const uint64_t target = p.recv_target();   // ring position 3456
    // The cursor lives in absolute frames; after laps the same ring position
    // must report the same error.
    const uint64_t lapped = target + 3ull * 4096;
    check(p.recv_error(lapped) == 0, "error: laps of the ring are equivalent");
}

// The resync window the daemon applies: forward, a read of [pos, pos+P) must
// not touch the live block at the write head; backward, one settled block of
// extra latency is the most we advertise.
int64_t forward_limit(const RingProjector& p) {
    return (int64_t)p.block_frames + p.jitter_frames
         - (int64_t)p.period_frames;
}
bool outside_window(const RingProjector& p, int64_t err) {
    return err > forward_limit(p) || err < -(int64_t)p.block_frames;
}

// Steady state: the HAL head advances by N every N/P daemon cycles, the
// cursor by P every cycle. For every seed phase — the cursor may start
// anywhere inside the window, because it starts wherever the last settled
// block plus cushion happens to be — the error must stay inside the window
// forever: no snap is ever needed.
void test_steady_state_never_resyncs() {
    struct { uint32_t N, P; } sizes[] = {{512, 64}, {1024, 64}, {64, 64},
                                          {64, 512}, {2048, 32}};
    for (const auto& s : sizes) {
        for (uint32_t phase = 0; phase < s.N / s.P; ++phase) {
            RingProjector p = make_proj(s.N, s.P, 128);
            p.hal_output_write_head = s.N;   // first block settled
            // Seed at target + phase*P: covers every phase the first active
            // cycle can land at relative to the head's group.
            uint64_t cursor = p.recv_target() + phase * s.P;
            int snaps = 0;
            for (int cycle = 0; cycle < 50000; ++cycle) {
                if (cycle % (s.N / s.P) == 0) p.hal_output_write_head += s.N;
                const int64_t err = p.recv_error(cursor);
                if (outside_window(p, err)) {
                    cursor += (uint64_t)(-err);
                    ++snaps;
                }
                cursor += s.P;
            }
            if (snaps != 0) {
                printf("  (N=%u P=%u phase=%u: %d snaps)\n",
                       s.N, s.P, phase, snaps);
                check(false, "steady state: free-run holds with no snaps");
            }
        }
    }
}

// A HAL stall: the head stops, the error runs negative, and the rule snaps
// the cursor back. This is the degradation path — it must converge, not run
// away, and the steady-state property must re-establish after the resume.
void test_stall_snaps_back_and_converges() {
    const uint32_t N = 512, P = 64;
    RingProjector p = make_proj(N, P, 128);
    p.hal_output_write_head = 10 * N;
    uint64_t cursor = p.recv_target();
    int snaps = 0;
    // Head stalls for 24 daemon cycles (3 HAL groups), then resumes.
    for (int cycle = 0; cycle < 3000; ++cycle) {
        if (cycle >= 24 && (cycle - 24) % (N / P) == 0) {
            p.hal_output_write_head += N;
        }
        const int64_t err = p.recv_error(cursor);
        if (outside_window(p, err)) {
            cursor += (uint64_t)(-err);
            ++snaps;
        }
        cursor += P;
    }
    check(snaps > 0, "stall: snaps occur while the head is stopped");
    const int64_t finalErr = p.recv_error(cursor);
    check(!outside_window(p, finalErr),
          "stall: error back inside the window after resume");
}

// A rate error: the head advances N per group but the cursor advances
// slightly more. The error must march forward until the rule snaps — this
// is the drift-detection property recvResyncs exists to surface, so the
// rule must not mask it by, e.g., a window wide enough to absorb it.
void test_rate_error_snaps_periodically() {
    const uint32_t N = 512, P = 64;
    RingProjector p = make_proj(N, P, 128);
    p.hal_output_write_head = N;
    uint64_t cursor = p.recv_target();
    int snaps = 0;
    for (int cycle = 0; cycle < 50000; ++cycle) {
        if (cycle % (N / P) == 0) p.hal_output_write_head += N;
        const int64_t err = p.recv_error(cursor);
        if (outside_window(p, err)) {
            cursor += (uint64_t)(-err);
            ++snaps;
        }
        cursor += P + 1;   // one frame per cycle faster than the head
    }
    check(snaps > 1, "rate error: cursor snapped more than once (drift visible)");
}

// The window edges: exactly at the limit is not a fault; one past it is.
void test_window_boundaries() {
    RingProjector p = make_proj();
    const uint32_t target = p.recv_target();
    const int64_t fwd = forward_limit(p);
    check(p.recv_error(target + fwd) == fwd,
          "window: at the forward limit, not yet a fault");
    check(outside_window(p, fwd + 1),
          "window: one past the forward limit is a fault");
    check(p.recv_error(target - p.block_frames) == -((int64_t)p.block_frames),
          "window: at -block, not yet a fault");
    check(outside_window(p, -(int64_t)p.block_frames - 1),
          "window: one past -block is a fault");
}

// send_offset() is unchanged by this design; pin it so the downstream path
// cannot regress silently.
void test_send_offset_unaffected() {
    RingProjector p = make_proj();
    p.hal_input_read_head = 1000;
    p.frame_cursor = 1000;
    p.last_synced_read_frame = 1000;
    // head + block + jitter + delta
    check(p.send_offset() == (1000 + 512 + 128) % 4096,
          "send: one block plus cushion ahead of the read head");
    p.frame_cursor = 1000 + 64;   // one JACK cycle past the sync
    check(p.send_offset() == (1000 + 512 + 128 + 64) % 4096,
          "send: open-loop delta advances through a stall");
}

} // namespace

int main() {
    const std::function<void()> tests[] = {
        test_target_is_one_block_plus_jitter_behind_head,
        test_error_sign_and_magnitude,
        test_error_wraps_the_ring_shortest_way,
        test_error_is_exact_on_absolute_frames,
        test_steady_state_never_resyncs,
        test_stall_snaps_back_and_converges,
        test_rate_error_snaps_periodically,
        test_window_boundaries,
        test_send_offset_unaffected,
    };
    for (const auto& t : tests) t();
    if (g_failures == 0) {
        printf("projector: all %zu tests passed\n", sizeof(tests) / sizeof(tests[0]));
        return 0;
    }
    printf("projector: %d failure(s)\n", g_failures);
    return 1;
}