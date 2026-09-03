/*
 File: JackBridge.h

 MIT License
 
 Copyright (c) 2018 Shunji Uno <madhatter68@linux-dtm.ivory.ne.jp>
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 */
#pragma once
#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <sstream>

#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdint.h>
#include <atomic>
#include <mach/mach_time.h>
#include "jb_log.hpp"

// IPC contract version. Bump on every shm layout change (sizes, offsets, field
// types, sync semantics). Phase 2.3 wires the handshake — daemon and HAL both
// refuse to attach on mismatch.
#define JACKBRIDGE_PROTOCOL_VERSION 13

// shm sync fields are std::atomic<uint64_t> placed by reinterpret_cast over the
// mapped region. Both targets must agree that the type is lock-free and the
// representation is just an aligned uint64_t — true on every arm64 / x86_64
// target we ship to, but assert it at compile time so a future toolchain
// surprise fails loudly instead of silently corrupting the IPC.
static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t),
              "std::atomic<uint64_t> must have the same layout as uint64_t");
static_assert(alignof(std::atomic<uint64_t>) == alignof(uint64_t),
              "std::atomic<uint64_t> must have the same alignment as uint64_t");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "std::atomic<uint64_t> must be lock-free on this target");

/******************************************************************************
 Audio functions (Generic/CoreAudio)
******************************************************************************/
// FIELD OWNERSHIP. Every control field below has a defined writer. Everyone
// else reads it. The fault bitfield is the exception at field granularity:
// its bits have separate owners. This is load-bearing; breaking it cost a
// permanent-silence bug when the daemon wrote DRIVER_STATUS = INIT from
// on_shutdown and the replacement daemon then zeroed its output forever.
//
//   Driver (HAL) owns : DRIVER_STATUS, HAL_ANCHOR_*,
//                       HAL_INPUT_READ_HEAD, HAL_OUTPUT_WRITE_HEAD,
//                       HAL_NFRAMES, HAL_SAMPLE_RATE,
//                       READ/WRITE_FRAME_NUMBER(i), DRIVER_FAULT bit 0,
//                       HAL_INPUT_STARVE_BLOCKS, HAL_INPUT_STARVE_FRAMES
//   Daemon owns       : DAEMON_ALIVE, SLAVE_PORTS_CONNECTED, DAEMON_XRUNS,
//                       JITTER_FRAMES, NET_LATENCY_CYCLES, NET_RING_FRAMES,
//                       HEALTH_DELTA_MAX, HEALTH_SNAPS, REANCHOR_COUNT,
//                       DUP_READ_CYCLES, SKIP_READ_FRAMES, DUP_WRITE_CYCLES,
//                       SKIP_WRITE_FRAMES, RECV_RESYNCS, SEND_RESYNCS,
//                       DAEMON_SEND_CURSOR, DAEMON_RECV_CURSOR,
//                       DRIVER_FAULT bit 1
//   App owns          : RESYNC_REQUEST (write-only; the driver and the daemon
//                       both read it and re-anchor their own side)
//   Mode-dependent    : NUMBER_TIMESTAMPS, ZERO_HOST_TIME, SEED. SYNC_MODE
//                       picks the writer: 1 (the only mode shipped -- the
//                       daemon sets it unconditionally) means the daemon
//                       publishes the timeline and GetZeroTimeStamp just
//                       relays it; 0 means the HAL free-runs and writes them
//                       itself. Never both.
//
// The app is a reader everywhere else. If you need to signal across the
// boundary, add a request field owned by the sender -- do not write the
// receiver's state for it.

// Shared memory map: (mapped every 1MB boundary for each instance)
// 0x0000      : Control Registers (Read/Write Pointers)
// 0x0000      :    upstream write pointer
// 0x0002      :    upstream read  pointer
// 0x0004      :    downstream write pointer
// 0x0006      :    downstream read pointer
// 0x0080      :    RingBufferSize(Default: 4K*2ch)
// 0x0100      :    TimeStamp number
// 0x0108      :    HostTime at recent TimeZero
// 0x0110      :    Seed
// 0x0118      :    SyncMode
// 0x0120      :    RingBufferSize
// 0x0128      :    Driver status
// 0x0130      :    Protocol version (handshake — refuse-on-mismatch)
// 0x0138      :    Daemon alive heartbeat counter
// 0x0140      :    HAL anchor seqlock counter (even=stable, odd=write in progress)
// 0x0148      :    HAL anchor mCurrentTime.mHostTime
// 0x0150      :    HAL anchor mCurrentTime.mSampleTime
// 0x0158      :    HAL input read head (mInputTime.mSampleTime, frames)
// 0x0160      :    HAL output write head (mOutputTime.mSampleTime, frames)
// 0x0168      :    HAL current cycle nframes
// 0x0170      :    HAL current sample rate
// 0x0180      :    Current Frame Number(coreAudio read, stream 0)
// 0x0188      :    Current Frame Number(coreAudio write, stream 0)
// 0x0190      :    Current Frame Number(coreAudio read, stream 1)
// 0x0198      :    Current Frame Number(coreAudio write, stream 1)
// 0x01a0      :    JACK period frames the daemon observed (P, latency model)
// 0x01a8      :    JACK sample rate the daemon observed (f_s, latency model)
// 0x01b0      :    slave ports connected to a live peer (daemon, 0..6)
// 0x01b8      :    daemon xrun counter (monotonic; today only reaches os_log)
// 0x01c0      :    driver fault bitfield (driver; bit 0 = mDeviceIsAlive false)
// 0x01c8      :    app -> driver re-anchor request (app increments; driver acts + echoes)
// 0x01d0      :    daemon write-ahead safety margin, frames (JitterFrames)
// 0x01d8      :    netadapter -l the pi runs, in cycles (daemon, from config.plist)
// 0x01e0      :    netadapter -g the pi runs, in frames (daemon, from config.plist)
// 0x01e8      :    health: peak timeline deficit in the last window, frames (daemon)
// 0x01f0      :    health: snap count in the last window (daemon)
// 0x01f8      :    re-anchors performed since daemon start (daemon)
// 0x0200      :    CoreAudio device name (daemon-published, NUL-terminated UTF-8)
// 0x0280      :    cadence: daemon cycles that re-read an unchanged write head (daemon)
// 0x0288      :    cadence: frames the write head skipped past one block (daemon)
// 0x0290      :    cadence: daemon cycles that re-wrote against an unchanged read head (daemon)
// 0x0298      :    cadence: frames the read head skipped past one block (daemon)
// 0x02a0      :    cadence: upstream cursor snap-to-target corrections (daemon)
// 0x02a8      :    cadence: downstream cursor snap-to-target corrections (daemon)
// 0x02b0      :    daemon send cursor, absolute frames (daemon)
// 0x02b8      :    daemon recv cursor, absolute frames (daemon)
// 0x02c0      :    HAL capture blocks read past the send cursor (driver)
// 0x02c8      :    HAL capture frames read past the send cursor (driver)
// 0x10000     : Upstream buffer #0 (Driver -> Application)
// 0x18000     : Downstream buffer #0 (Application -> Driver)
// 0x20000     : Upstream buffer #0 (Driver -> Application)
// 0x28000     : Downstream buffer #0 (Application -> Driver)

// Byte offsets of the control fields within the shm region. Single source of
// truth — attach_shm() and any external read-only reader (e.g. the
// PiStompCompanion menu-bar app) must use these, not hand-copied literals.
// Mirrors the map comment above. Layout changes here require a
// JACKBRIDGE_PROTOCOL_VERSION bump.
#define JB_OFF_NUMBER_TIMESTAMPS   (0x100)
#define JB_OFF_ZERO_HOST_TIME      (0x108)
#define JB_OFF_SEED                (0x110)
#define JB_OFF_SYNC_MODE           (0x118)
#define JB_OFF_BUFFER_SIZE         (0x120)
#define JB_OFF_DRIVER_STATUS       (0x128)
#define JB_OFF_PROTOCOL_VERSION    (0x130)
#define JB_OFF_DAEMON_ALIVE        (0x138)
#define JB_OFF_HAL_ANCHOR_SEQ      (0x140)
#define JB_OFF_HAL_ANCHOR_HOSTTIME (0x148)
#define JB_OFF_HAL_ANCHOR_SAMPLETIME (0x150)
#define JB_OFF_HAL_INPUT_READ_HEAD (0x158)
#define JB_OFF_HAL_OUTPUT_WRITE_HEAD (0x160)
#define JB_OFF_HAL_NFRAMES         (0x168)
#define JB_OFF_HAL_SAMPLE_RATE     (0x170)
// Per-stream frame cursors, written by the HAL IO thread every cycle.
#define JB_OFF_READ_FRAME_NUMBER(i)  (0x180+(i)*0x10)
#define JB_OFF_WRITE_FRAME_NUMBER(i) (0x188+(i)*0x10)
// One past the last per-stream counter. New atomic fields go here, not at a
// hand-picked address — the frame-number block grows with MAX_STREAMS.
#define JB_OFF_END_FRAME_NUMBERS   (JB_OFF_WRITE_FRAME_NUMBER(MAX_STREAMS-1)+8)

// Runtime timing the daemon discovers from its JACK server and the HAL needs
// for the advertised-latency model. Written once by the daemon at attach, read
// by the HAL at device open and again at StartIO.
#define JB_OFF_JACK_PERIOD_FRAMES  (0x1a0)
#define JB_OFF_JACK_SAMPLE_RATE    (0x1a8)

// Self-healing / honest-status fields (protocol 8). See docs/idiosyncrasies.md.
//   SLAVE_PORTS_CONNECTED  daemon: how many of its 6 slave ports are connected
//                          to a live peer right now. 0 while a corpse client
//                          still has ports registered but nothing services them.
//   DAEMON_XRUNS           daemon: monotonic xrun count (mXRunCount), published
//                          so the app can show "audio came back but is glitching"
//                          without tailing os_log.
//   DRIVER_FAULT           bitfield. The driver owns bit 0 (mDeviceIsAlive
//                          false); the daemon owns bit 1 (ring geometry
//                          invalid).
//   RESYNC_REQUEST         app -> driver AND daemon: the app stores a nonce
//                          here. The driver re-anchors gDevice and re-arms its
//                          liveness state in GetZeroTimeStamp; the daemon
//                          re-anchors the timeline, which is the half that
//                          matters under syncMode 1, and bumps REANCHOR_COUNT
//                          so the app can see that the request landed.
#define JB_OFF_SLAVE_PORTS_CONNECTED (0x1b0)
#define JB_OFF_DAEMON_XRUNS          (0x1b8)
#define JB_OFF_DRIVER_FAULT          (0x1c0)
#define JB_OFF_RESYNC_REQUEST        (0x1c8)

// Protocol-9. Frames the daemon stays ahead of the HAL's read head. The
// daemon reads JitterFrames from config.plist and publishes it here; the HAL
// reports it as kAudioDevicePropertySafetyOffset. One value, two consumers —
// a per-side constant is how the DAW's latency figure goes wrong silently.
#define JB_OFF_JITTER_FRAMES         (0x1d0)

// Protocol-10. The two netadapter loop parameters the pi actually runs. The
// Mac owns them: they live in config.plist, jackbridge-ctl writes them into
// /etc/default/jackbridge before each pi service start, and the daemon
// publishes them here so the HAL's latency model uses the live pair instead of
// the compile-time constants. A zero means the daemon has not published yet —
// readers fall back to JB_NET_LATENCY_CYCLES / JB_NETADAPTER_RING_FRAMES.
#define JB_OFF_NET_LATENCY_CYCLES    (0x1d8)
#define JB_OFF_NET_RING_FRAMES       (0x1e0)

// Protocol-10 timeline health. Both were os_log-only, which is why a stack
// could sit hours out of anchor and still show green everywhere a user or a
// script looks (docs/plan-tuning.md 2.9).
//   HEALTH_DELTA_MAX  daemon: peak (FrameNumber - mLastSyncedFrame) over the
//                     last ~5 s window, in frames. Near 0 in steady state.
//   HEALTH_SNAPS      daemon: snap count in that same window. Nonzero in
//                     steady state means the timeline is not holding.
//   REANCHOR_COUNT    daemon: monotonic count of re-anchors since start —
//                     HAL restarts, resync requests, and the automatic
//                     divergence re-anchor all bump it.
#define JB_OFF_HEALTH_DELTA_MAX      (0x1e8)
#define JB_OFF_HEALTH_SNAPS          (0x1f0)
#define JB_OFF_REANCHOR_COUNT        (0x1f8)

// Bit definitions for JB_OFF_DRIVER_FAULT. Writers own disjoint bits.
#define JB_FAULT_DEVICE_NOT_ALIVE   (1u << 0)
#define JB_FAULT_BAD_RING_GEOMETRY  (1u << 1)

// Device display name the HAL reports via kAudioObjectPropertyName (derived
// from PiHostname in config.plist). Bounded bytes plus a NUL, not a sync field.
//
// This lives past every atomic field on purpose. It used to start at 0x180,
// which is also where the per-stream frame counters live: the daemon wrote the
// name over the counters at attach, and the HAL's IO thread then wrote the
// counters back over the name. That only ever worked because the HAL happens
// to read the name once in _HW_Open before IO starts. The static_asserts below
// make the next such overlap a build error instead of a latent corruption.
#define JB_DEVICE_NAME_MAX         128
#define JB_OFF_DEVICE_NAME         (0x200)

// Cadence counters, all daemon-written, all monotonic.
//
// The daemon positions both rings relative to the HAL's heads, which move
// only when the HAL cycles. These counters record whether the daemon's own
// cycles ran 1:1 against them. recv reads destructively (RingCopy.hpp zeroes
// each slot as it copies it out), so the read counters are the audible ones:
// a repeat or a skip puts silence inside correctly-paced audio, which reads
// as a haze rather than as a click, while every timing field still looks
// clean.
//
//   DUP_READ_CYCLES    daemon cycles that re-read a slot they already zeroed;
//                      each one handed the pi silence.
//   SKIP_READ_FRAMES   frames no daemon cycle ever read.
//   DUP_WRITE_CYCLES   daemon cycles that re-wrote the same ring position.
//   SKIP_WRITE_FRAMES  ring positions the daemon left unwritten.
//   RECV_RESYNCS       corrections of the upstream read cursor. The cursor
//                      free-runs and is snapped to its target only when it
//                      leaves the safe window; every snap lands here so a
//                      correction can never hide in the counters above.
//
// Steady state is all five at 0 and staying there. Read them as rates, not
// totals: a handful accumulated across a start transient means nothing, and
// a counter that climbs while audio plays is the fault. A steady climb in
// RECV_RESYNCS specifically means the two clock rates differ — see
// docs/plan-free-running-cursor.md section 6.
#define JB_OFF_DUP_READ_CYCLES     (0x280)
#define JB_OFF_SKIP_READ_FRAMES    (0x288)
#define JB_OFF_DUP_WRITE_CYCLES    (0x290)
#define JB_OFF_SKIP_WRITE_FRAMES   (0x298)
#define JB_OFF_RECV_RESYNCS        (0x2a0)
#define JB_OFF_SEND_RESYNCS        (0x2a8)

// Protocol 13. The stock, not the transition. The four cadence counters above
// and RECV/SEND_RESYNCS all record *events*; none of them says how full the
// ring is, so a boundary holding with no margin reads identically to one
// holding comfortably. These two publish the daemon's absolute cursors, which
// with the HAL heads give the live ring occupancy on both sides.
#define JB_OFF_DAEMON_SEND_CURSOR  (0x2b0)
#define JB_OFF_DAEMON_RECV_CURSOR  (0x2b8)

// Driver-owned capture starvation. The HAL cannot otherwise tell a fresh
// block from one a lap old: below the ~426ms heartbeat threshold it memcpy's
// whatever the ring holds, so a short daemon stall put stale audio into the
// DAW's take with nothing recording that it happened. A block whose end lies
// past the published send cursor was not fully written when it was read.
#define JB_OFF_HAL_INPUT_STARVE_BLOCKS (0x2c0)
#define JB_OFF_HAL_INPUT_STARVE_FRAMES (0x2c8)
#define JB_DEVICE_NAME_FALLBACK    "pi-Stomp"

/******************************************************************************
 Advertised latency model
 ------------------------
 One-way leg, pi codec <-> Mac HAL, in frames. Both sides compute it from the
 same function so the daemon's log and the HAL's kAudioDevicePropertyLatency
 can never disagree. Full derivation and the per-hop table: docs/LATENCY-MODEL.md.

     T_adc/T_dac  codec group delay                  JB_CODEC_GROUP_DELAY_FRAMES
     T_alsa       pi ALSA capture (-n periods)       JB_ALSA_PERIODS_PI * P
     T_pj         pi jackd cycle                     P
     T_g          netadapter slip ring, steady state JB_NETADAPTER_RING_FRAMES / 2
     T_l          netadapter -l cycles of cushion    JB_NET_LATENCY_CYCLES * P
     T_wire       UDP transit, direct cable          JB_WIRE_TRANSIT_MICROS * f_s
     T_nm         Mac netmanager cycle               P
     T_mj         Mac jackd cycle                    P

 P is the JACK period. netJACK2 requires P_pi == P_mac, so one P covers both
 sides. T_g uses G/2 because the slip-ring controller resamples toward the ring
 midpoint; the full G is burst headroom, not steady-state latency.

 P and f_s are discovered from the Pi at startup, so this must be evaluated at
 runtime — a compile-time constant would only be right at the reference config
 (P=64, f_s=48000, which yields 722).
******************************************************************************/
// jackd -n on the pi (pistomp-arch jackdrc).
#define JB_ALSA_PERIODS_PI          2
// netadapter -l (cycles) and -g (frames). Both are passed explicitly by
// jackbridge-pi-up's jack_load; neither is a netadapter default any more.
//
// These two are FALLBACKS ONLY, for a reader that has no published pair yet.
// The live values are Mac-owned: NetLatency / NetRing in config.plist ->
// jackbridge-ctl writes /etc/default/jackbridge -> jackbridge-pi-up passes
// them to netadapter, and the daemon publishes the same pair into
// JB_OFF_NET_LATENCY_CYCLES / JB_OFF_NET_RING_FRAMES. Keep these equal to the
// config.plist defaults so a first boot with no published pair advertises the
// number the link will actually cost. L=2 with G=512 was recorded as unstable
// under mod-host load (2026-08-30); L=4 / G=1024 is the stable pair.
#define JB_NET_LATENCY_CYCLES       4
#define JB_NETADAPTER_RING_FRAMES   1024
// Daemon write-ahead safety margin in frames. Reported via
// kAudioDevicePropertySafetyOffset; the DAW adds it on top of the one-way
// latency, so it must NOT be included in jb_one_way_latency_frames().
// This is a real latency cost, not free headroom: the daemon positions its
// cursors one block plus JB_JITTER_FRAMES clear of each HAL head. J=0 is the
// tightest correct alignment. The ring must still satisfy the strict daemon
// safety condition `ring_frames > 2 * max(N, P) + J`; with this 4096-frame
// ring and J=128, the supported maximum is below 1984 frames.
//
// 128 chosen 2026-08-30. The previous 320 was sized to cover the maximum
// observed CoreAudio IOProc burst (maxBurst=272 in a 2-hour live session).
// 128 is below that figure, so a burst past 128 frames can starve the ring;
// this trades that risk for 4 ms per direction. If starvation appears under
// load, raise it before you change anything else.
#define JB_JITTER_FRAMES            128
// IQaudIO ADC/DAC group delay (datasheet, low ms -> ~1 frame).
#define JB_CODEC_GROUP_DELAY_FRAMES 1
// LAN one-way transit on a direct cable. A consumer switch in the path costs
// more (0.5-1 ms); this constant assumes the supported direct-cable topology.
#define JB_WIRE_TRANSIT_MICROS      354
// Reference config used when the daemon has not published its timing yet.
#define JB_REFERENCE_PERIOD_FRAMES  64
#define JB_REFERENCE_SAMPLE_RATE    48000

// Plausibility bounds. Anything outside these means we are reading a stale or
// uninitialized shm field, so fall back to the reference config rather than
// advertise nonsense to the DAW.
static inline bool jb_timing_is_plausible(uint64_t period_frames, uint64_t sample_rate) {
    return period_frames >= 16 && period_frames <= 8192 &&
           sample_rate   >= 8000 && sample_rate   <= 384000;
}

// Plausibility bounds for the netadapter loop pair, same purpose as
// jb_timing_is_plausible: a zero (or nonsense) field means "not published
// yet", and the caller must fall back to the compile-time constant rather
// than advertise a wrong number to the DAW. The upper bounds mirror the
// clamps in jackbridge-pi-up.
static inline bool jb_net_pair_is_plausible(uint64_t net_latency_cycles,
                                            uint64_t net_ring_frames) {
    return net_latency_cycles >= 1 && net_latency_cycles <= 30 &&
           net_ring_frames    >= 64 && net_ring_frames   <= 65536;
}

// One-way leg in frames: pi ADC -> Mac HAL, or Mac HAL -> pi DAC. The two
// directions are symmetric (each carries exactly one codec pass), so the
// monitoring round trip is 2x this.
//
// L and G are the netadapter pair the pi is actually running. The two-argument
// form below fills them in from the compile-time constants; every caller that
// can read the published pair should use this form instead, because the
// constants are only right when nobody has retuned the link.
static inline uint32_t jb_one_way_latency_frames(uint64_t period_frames, uint64_t sample_rate,
                                                 uint64_t net_latency_cycles,
                                                 uint64_t net_ring_frames) {
    if (!jb_timing_is_plausible(period_frames, sample_rate)) {
        period_frames = JB_REFERENCE_PERIOD_FRAMES;
        sample_rate   = JB_REFERENCE_SAMPLE_RATE;
    }
    if (!jb_net_pair_is_plausible(net_latency_cycles, net_ring_frames)) {
        net_latency_cycles = JB_NET_LATENCY_CYCLES;
        net_ring_frames    = JB_NETADAPTER_RING_FRAMES;
    }
    // T_alsa + T_pj + T_l + T_nm + T_mj, all integer multiples of the period.
    const uint64_t period_terms =
        (JB_ALSA_PERIODS_PI + net_latency_cycles + 3) * period_frames;
    // T_wire, rounded to the nearest frame at this sample rate.
    const uint64_t wire =
        (sample_rate * JB_WIRE_TRANSIT_MICROS + 500000ULL) / 1000000ULL;
    return (uint32_t)(JB_CODEC_GROUP_DELAY_FRAMES +
                      period_terms +
                      (net_ring_frames / 2) +
                      wire);
}

static inline uint32_t jb_one_way_latency_frames(uint64_t period_frames, uint64_t sample_rate) {
    return jb_one_way_latency_frames(period_frames, sample_rate,
                                     JB_NET_LATENCY_CYCLES, JB_NETADAPTER_RING_FRAMES);
}

// Monitoring trip: pi ADC -> Mac -> pi DAC. What a guitarist monitoring
// through the Mac actually hears, excluding the DAW's own buffers.
static inline uint32_t jb_monitoring_trip_frames(uint64_t period_frames, uint64_t sample_rate,
                                                 uint64_t net_latency_cycles,
                                                 uint64_t net_ring_frames) {
    return 2 * jb_one_way_latency_frames(period_frames, sample_rate,
                                         net_latency_cycles, net_ring_frames);
}

static inline uint32_t jb_monitoring_trip_frames(uint64_t period_frames, uint64_t sample_rate) {
    return 2 * jb_one_way_latency_frames(period_frames, sample_rate);
}

typedef float sample_t;
#define AUDIO_SAMPLE_SIZE (sizeof(sample_t))
// pi-stomp recording layout: 4-in (HW capture L/R + mod-host wet L/R) / 2-out.
// Two stereo input streams, one stereo output stream. MAX_STREAMS stays 2 —
// shm region size is unchanged, only the direction split moves.
#define NUM_INPUT_STREAMS   2
#define NUM_OUTPUT_STREAMS  1
#define MAX_STREAMS         2
#define MAX_CHANNELS        ((MAX_STREAMS)*2)
#define NUM_INSTANCES       1

#define STRBUFSZ            (0x8000) // 32KB Ring buffer
#define STRBUFNUM           (STRBUFSZ/AUDIO_SAMPLE_SIZE) // 1024 entries
#define REGSMAP_SIZE        (0x10000*(MAX_STREAMS)+0x10000)
#define REGSMAP_BOUNDARY    REGSMAP_SIZE
#define JACK_SHMSIZE        (REGSMAP_SIZE*NUM_INSTANCES)
#define STRBUF_U0           (0x10000)
#define STRBUF_UP(i)        (0x10000*(i)+0x10000)
#define STRBUF_DOWN(i)      (0x10000*(i)+0x18000)

// Control-region layout guards. The region is laid out by hand with literal
// offsets, so nothing but these asserts stops two fields from claiming the
// same bytes — which is exactly what happened between the device name and the
// per-stream frame counters before protocol 7. Each new field belongs here.
static_assert(JB_OFF_JACK_PERIOD_FRAMES >= JB_OFF_END_FRAME_NUMBERS,
              "JACK timing fields overlap the per-stream frame counters");
static_assert(JB_OFF_JACK_SAMPLE_RATE >= JB_OFF_JACK_PERIOD_FRAMES + 8,
              "JACK sample rate overlaps the JACK period field");
static_assert(JB_OFF_SLAVE_PORTS_CONNECTED >= JB_OFF_JACK_SAMPLE_RATE + 8,
              "slave-ports-connected overlaps the JACK sample rate field");
static_assert(JB_OFF_DAEMON_XRUNS   >= JB_OFF_SLAVE_PORTS_CONNECTED + 8 &&
              JB_OFF_DRIVER_FAULT    >= JB_OFF_DAEMON_XRUNS + 8 &&
              JB_OFF_RESYNC_REQUEST  >= JB_OFF_DRIVER_FAULT + 8,
              "protocol-8 self-healing fields overlap");
static_assert((JB_OFF_SLAVE_PORTS_CONNECTED % 8) == 0 &&
              (JB_OFF_DAEMON_XRUNS % 8) == 0 &&
              (JB_OFF_DRIVER_FAULT % 8) == 0 &&
              (JB_OFF_RESYNC_REQUEST % 8) == 0,
              "protocol-8 atomic<uint64_t> fields must be 8-byte aligned");
static_assert(JB_OFF_JITTER_FRAMES >= JB_OFF_RESYNC_REQUEST + 8 &&
              (JB_OFF_JITTER_FRAMES % 8) == 0,
              "jitter-frames field overlaps or is misaligned");
static_assert(JB_OFF_NET_LATENCY_CYCLES >= JB_OFF_JITTER_FRAMES + 8 &&
              JB_OFF_NET_RING_FRAMES     >= JB_OFF_NET_LATENCY_CYCLES + 8 &&
              JB_OFF_HEALTH_DELTA_MAX    >= JB_OFF_NET_RING_FRAMES + 8 &&
              JB_OFF_HEALTH_SNAPS        >= JB_OFF_HEALTH_DELTA_MAX + 8 &&
              JB_OFF_REANCHOR_COUNT      >= JB_OFF_HEALTH_SNAPS + 8,
              "protocol-10 fields overlap");
static_assert((JB_OFF_NET_LATENCY_CYCLES % 8) == 0 &&
              (JB_OFF_NET_RING_FRAMES % 8) == 0 &&
              (JB_OFF_HEALTH_DELTA_MAX % 8) == 0 &&
              (JB_OFF_HEALTH_SNAPS % 8) == 0 &&
              (JB_OFF_REANCHOR_COUNT % 8) == 0,
              "protocol-10 atomic<uint64_t> fields must be 8-byte aligned");
static_assert(JB_OFF_DEVICE_NAME >= JB_OFF_REANCHOR_COUNT + 8,
              "device name overlaps the control atomics");
static_assert(JB_OFF_DEVICE_NAME + JB_DEVICE_NAME_MAX <= STRBUF_U0,
              "device name runs into the first ring buffer");
static_assert(JB_OFF_DUP_READ_CYCLES >= JB_OFF_DEVICE_NAME + JB_DEVICE_NAME_MAX,
              "cadence counters overlap the device name");
static_assert(JB_OFF_RECV_RESYNCS >= JB_OFF_SKIP_WRITE_FRAMES + 8 &&
              (JB_OFF_RECV_RESYNCS % 8) == 0,
              "resync counter overlaps or is misaligned");
static_assert(JB_OFF_SEND_RESYNCS        >= JB_OFF_RECV_RESYNCS + 8 &&
              JB_OFF_DAEMON_SEND_CURSOR  >= JB_OFF_SEND_RESYNCS + 8 &&
              JB_OFF_DAEMON_RECV_CURSOR  >= JB_OFF_DAEMON_SEND_CURSOR + 8 &&
              JB_OFF_HAL_INPUT_STARVE_BLOCKS >= JB_OFF_DAEMON_RECV_CURSOR + 8 &&
              JB_OFF_HAL_INPUT_STARVE_FRAMES >= JB_OFF_HAL_INPUT_STARVE_BLOCKS + 8 &&
              (JB_OFF_SEND_RESYNCS % 8) == 0 &&
              (JB_OFF_DAEMON_SEND_CURSOR % 8) == 0 &&
              (JB_OFF_DAEMON_RECV_CURSOR % 8) == 0 &&
              (JB_OFF_HAL_INPUT_STARVE_BLOCKS % 8) == 0 &&
              (JB_OFF_HAL_INPUT_STARVE_FRAMES % 8) == 0,
              "protocol 13 fields must not overlap and must stay 8-byte aligned");
static_assert(JB_OFF_HAL_INPUT_STARVE_FRAMES + 8 <= STRBUF_U0,
              "resync counter runs into the first ring buffer");
static_assert(JB_OFF_SKIP_READ_FRAMES  >= JB_OFF_DUP_READ_CYCLES + 8 &&
              JB_OFF_DUP_WRITE_CYCLES  >= JB_OFF_SKIP_READ_FRAMES + 8 &&
              JB_OFF_SKIP_WRITE_FRAMES >= JB_OFF_DUP_WRITE_CYCLES + 8,
              "cadence counters overlap");
static_assert((JB_OFF_DUP_READ_CYCLES % 8) == 0 &&
              (JB_OFF_SKIP_READ_FRAMES % 8) == 0 &&
              (JB_OFF_DUP_WRITE_CYCLES % 8) == 0 &&
              (JB_OFF_SKIP_WRITE_FRAMES % 8) == 0,
              "cadence atomic<uint64_t> fields must be 8-byte aligned");
static_assert(JB_OFF_SKIP_WRITE_FRAMES + 8 <= STRBUF_U0,
              "cadence counters run into the first ring buffer");
static_assert((JB_OFF_JACK_PERIOD_FRAMES % 8) == 0 &&
              (JB_OFF_JACK_SAMPLE_RATE % 8) == 0,
              "atomic<uint64_t> fields must be 8-byte aligned");

#define JACK_SHMPATH        "/JackBridge"

class JackBridgeDriverIF {
protected:
    uint32_t instance;
    int shm_fd;
    sample_t *buf_up[MAX_STREAMS];
    sample_t *buf_down[MAX_STREAMS];
    uint64_t   FrameNumber;
    int        RingFrames;          // shm ring size in frames (STRBUFNUM/2 = 4096)
    std::atomic<uint64_t> *shmNumberTimeStamps;
    std::atomic<uint64_t> *shmZeroHostTime;
    std::atomic<uint64_t> *shmSeed;
    std::atomic<uint64_t> *shmSyncMode;
    std::atomic<uint64_t> *shmBufferSize;
    std::atomic<uint64_t> *shmDriverStatus;
#define JB_DRV_STATUS_INIT      0
#define JB_DRV_STATUS_ACTIVE    1
#define JB_DRV_STATUS_STARTED   2
    std::atomic<uint64_t> *shmProtocolVersion;
    std::atomic<uint64_t> *shmDaemonAlive;
    // HAL-authoritative anchor — published by the HAL IO thread once per cycle
    // under a seqlock, consumed by the daemon to project its read/write heads
    // into the HAL's current window. Step 2 of the sync rework. Fields are
    // valid iff shmHalAnchorSeq is even on both reads of a snapshot.
    std::atomic<uint64_t> *shmHalAnchorSeq;
    std::atomic<uint64_t> *shmHalAnchorHostTime;
    std::atomic<uint64_t> *shmHalAnchorSampleTime;
    std::atomic<uint64_t> *shmHalInputReadHead;
    std::atomic<uint64_t> *shmHalOutputWriteHead;
    std::atomic<uint64_t> *shmHalNFrames;
    std::atomic<uint64_t> *shmHalSampleRate;
    // Human-readable CoreAudio device name, written once by the daemon at
    // attach. Plain bytes — read once by the HAL at device open, so no
    // atomics/seqlock.
    char (*shmDeviceName)[JB_DEVICE_NAME_MAX];
    // Daemon-observed JACK timing, consumed by the HAL's latency model.
    std::atomic<uint64_t> *shmJackPeriodFrames;
    std::atomic<uint64_t> *shmJackSampleRate;
    // Protocol-8 self-healing fields. See the JB_OFF_* comments above.
    std::atomic<uint64_t> *shmSlavePortsConnected; // daemon writes
    std::atomic<uint64_t> *shmDaemonXRuns;         // daemon writes
    std::atomic<uint64_t> *shmRecvResyncs;         // daemon writes (upstream cursor corrections)
    std::atomic<uint64_t> *shmSendResyncs;         // daemon writes (downstream cursor corrections)
    std::atomic<uint64_t> *shmDaemonSendCursor;    // daemon writes (absolute frames)
    std::atomic<uint64_t> *shmDaemonRecvCursor;    // daemon writes (absolute frames)
    std::atomic<uint64_t> *shmHalInputStarveBlocks;// driver writes
    std::atomic<uint64_t> *shmHalInputStarveFrames;// driver writes
    std::atomic<uint64_t> *shmDriverFault;         // driver bit 0, daemon bit 1
    std::atomic<uint64_t> *shmResyncRequest;       // app writes, driver echoes
    std::atomic<uint64_t> *shmJitterFrames;        // daemon writes (protocol 9)
    // Protocol-10. The live netadapter pair and the timeline-health window.
    std::atomic<uint64_t> *shmNetLatencyCycles;    // daemon writes
    std::atomic<uint64_t> *shmNetRingFrames;       // daemon writes
    std::atomic<uint64_t> *shmHealthDeltaMax;      // daemon writes
    std::atomic<uint64_t> *shmHealthSnaps;         // daemon writes
    std::atomic<uint64_t> *shmReanchorCount;       // daemon writes
    // Cadence counters (daemon writes; see the block above).
    std::atomic<uint64_t> *shmDupReadCycles;
    std::atomic<uint64_t> *shmSkipReadFrames;
    std::atomic<uint64_t> *shmDupWriteCycles;
    std::atomic<uint64_t> *shmSkipWriteFrames;
    std::atomic<uint64_t> *shmReadFrameNumber[MAX_STREAMS];
    std::atomic<uint64_t> *shmWriteFrameNumber[MAX_STREAMS];

    int create_shm() {
        struct stat stat;
        JB_LOG_INFO(jb_log_shm(), "JackBridge: Initializing shared memory to communicate with jack(%d).", 0);
        shm_fd = shm_open(JACK_SHMPATH, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
        if (shm_fd < 0) {
            JB_LOG_ERR(jb_log_shm(), "shm cannot be opened with %{public}s.", strerror(errno));
            return -1;
        }
        
        if (fstat(shm_fd, &stat) < 0) {
            JB_LOG_ERR(jb_log_shm(), "Couldn't get shm stat with %{public}s.", strerror(errno));
            close(shm_fd);
            return -1;
        }
        
        if (stat.st_size != JACK_SHMSIZE) {
            if (ftruncate(shm_fd, JACK_SHMSIZE) == -1) {
                JB_LOG_INFO(jb_log_shm(), "shm cannot be truncated with %{public}s. Try to recreate shm.", strerror(errno));
                close(shm_fd);
                shm_unlink(JACK_SHMPATH);
                shm_fd = shm_open(JACK_SHMPATH, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
                if (shm_fd < 0) {
                    JB_LOG_ERR(jb_log_shm(), "shm cannot be recreated with %{public}s.", strerror(errno));
                    return -1;
                }
                if (ftruncate(shm_fd, JACK_SHMSIZE) == -1) {
                    JB_LOG_INFO(jb_log_shm(), "shm cannot be truncated with %{public}s.", strerror(errno));
                }
            }
            JB_LOG_INFO(jb_log_shm(), "Recreated shm because shm size is not matched as expected. (%d)", 0);
        }
        close(shm_fd);
        return 0;
    }
    
    int attach_shm() {
        struct stat stat;
        
        shm_fd = shm_open(JACK_SHMPATH, O_RDWR, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
        if (shm_fd < 0) {
            JB_LOG_ERR(jb_log_shm(), "shm_open() failed with %{public}s.", strerror(errno));
            return -1;
        }
        
        if (fstat(shm_fd, &stat) < 0) {
            JB_LOG_ERR(jb_log_shm(), "fstat() failed with %{public}s.", strerror(errno));
            return -1;
        } else {
            if (stat.st_size != JACK_SHMSIZE) {
                JB_LOG_ERR(jb_log_shm(), "does not match shmsize(%lld). May be driver version mismatch", stat.st_size);
            }
        }
        
        char* shm_base = (char*)mmap(NULL, REGSMAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, shm_fd, instance*REGSMAP_BOUNDARY);
        //char* shm_base = (char*)mmap(NULL, JACK_SHMSIZE, PROT_READ|PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (shm_base == MAP_FAILED) {
            JB_LOG_ERR(jb_log_shm(), "mmap() failed with %{public}s", strerror(errno));
            return -1;
        }

        shmNumberTimeStamps = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_NUMBER_TIMESTAMPS);
        shmZeroHostTime = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_ZERO_HOST_TIME);
        shmSeed = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_SEED);
        shmSyncMode = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_SYNC_MODE);
        shmBufferSize = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_BUFFER_SIZE);
        shmDriverStatus = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_DRIVER_STATUS);
        shmProtocolVersion = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_PROTOCOL_VERSION);
        shmDaemonAlive = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_DAEMON_ALIVE);
        shmHalAnchorSeq        = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_HAL_ANCHOR_SEQ);
        shmHalAnchorHostTime   = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_HAL_ANCHOR_HOSTTIME);
        shmHalAnchorSampleTime = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_HAL_ANCHOR_SAMPLETIME);
        shmHalInputReadHead    = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_HAL_INPUT_READ_HEAD);
        shmHalOutputWriteHead  = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_HAL_OUTPUT_WRITE_HEAD);
        shmHalNFrames          = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_HAL_NFRAMES);
        shmHalSampleRate       = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_HAL_SAMPLE_RATE);
        shmDeviceName          = reinterpret_cast<char(*)[JB_DEVICE_NAME_MAX]>(shm_base+JB_OFF_DEVICE_NAME);
        shmJackPeriodFrames    = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_JACK_PERIOD_FRAMES);
        shmJackSampleRate      = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_JACK_SAMPLE_RATE);
        shmSlavePortsConnected = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_SLAVE_PORTS_CONNECTED);
        shmDaemonXRuns         = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_DAEMON_XRUNS);
        shmDriverFault         = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_DRIVER_FAULT);
        shmResyncRequest       = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_RESYNC_REQUEST);
        shmJitterFrames        = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_JITTER_FRAMES);
        shmNetLatencyCycles    = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_NET_LATENCY_CYCLES);
        shmNetRingFrames       = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_NET_RING_FRAMES);
        shmHealthDeltaMax      = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_HEALTH_DELTA_MAX);
        shmHealthSnaps         = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_HEALTH_SNAPS);
        shmReanchorCount       = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_REANCHOR_COUNT);
        shmDupReadCycles       = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_DUP_READ_CYCLES);
        shmSkipReadFrames      = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_SKIP_READ_FRAMES);
        shmDupWriteCycles      = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_DUP_WRITE_CYCLES);
        shmSkipWriteFrames     = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_SKIP_WRITE_FRAMES);
        shmRecvResyncs         = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_RECV_RESYNCS);
        shmSendResyncs         = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_SEND_RESYNCS);
        shmDaemonSendCursor    = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_DAEMON_SEND_CURSOR);
        shmDaemonRecvCursor    = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_DAEMON_RECV_CURSOR);
        shmHalInputStarveBlocks= reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_HAL_INPUT_STARVE_BLOCKS);
        shmHalInputStarveFrames= reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_HAL_INPUT_STARVE_FRAMES);

        for(int i=0; i<MAX_STREAMS; i++) {
            buf_up[i]   = (sample_t*)(shm_base + STRBUF_UP(i));
            buf_down[i] = (sample_t*)(shm_base + STRBUF_DOWN(i));
            // +i*0x10 was missing on shmReadFrameNumber, aliasing stream 1 to
            shmReadFrameNumber[i]  = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_READ_FRAME_NUMBER(i));
            shmWriteFrameNumber[i] = reinterpret_cast<std::atomic<uint64_t>*>(shm_base+JB_OFF_WRITE_FRAME_NUMBER(i));
        }
        
        return 0;
    }
    
    // Cooperative version handshake. Whichever side attaches to a fresh shm
    // first publishes its version; the second side validates. Returns true on
    // match (or first-writer), false on mismatch — caller should log + exit.
    // The race window between the two CAS-like reads is benign: both sides
    // are pinned to the same JACKBRIDGE_PROTOCOL_VERSION at build time, so any
    // disagreement implies a stale shm from a previous install.
    bool check_protocol_version() {
        uint64_t observed = shmProtocolVersion->load(std::memory_order_acquire);
        if (observed == 0) {
            shmProtocolVersion->store(JACKBRIDGE_PROTOCOL_VERSION,
                                      std::memory_order_release);
            return true;
        }
        return observed == JACKBRIDGE_PROTOCOL_VERSION;
    }

public:
    JackBridgeDriverIF(uint32_t _instance) : instance(_instance) {
    }

    ~JackBridgeDriverIF() {
    }
};
