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
#define JACKBRIDGE_PROTOCOL_VERSION 8

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
// FIELD OWNERSHIP. Every control field below has exactly one writer. Everyone
// else reads it. This is not a style rule -- it is load-bearing, and breaking
// it cost a permanent-silence bug: the daemon wrote DRIVER_STATUS = INIT from
// on_shutdown, nothing but _HW_StartIO ever writes STARTED back, and so the
// replacement daemon read INIT and zeroed its output buffers forever while the
// link, the ports and the packets all looked healthy.
//
//   Driver (HAL) owns : DRIVER_STATUS, DRIVER_FAULT, HAL_ANCHOR_*,
//                       HAL_INPUT_READ_HEAD, HAL_OUTPUT_WRITE_HEAD,
//                       HAL_NFRAMES, HAL_SAMPLE_RATE,
//                       READ/WRITE_FRAME_NUMBER(i)
//   Daemon owns       : DAEMON_ALIVE, SLAVE_PORTS_CONNECTED, DAEMON_XRUNS,
//                       JACK_PERIOD_FRAMES, JACK_SAMPLE_RATE, DEVICE_NAME,
//                       BUFFER_SIZE, PROTOCOL_VERSION, SYNC_MODE
//   App owns          : RESYNC_REQUEST (write-only; the driver only reads it)
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
// 0x0200      :    CoreAudio device name (daemon-published, NUL-terminated UTF-8)
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
//   DRIVER_FAULT           driver: bitfield. bit 0 = mDeviceIsAlive == false
//                          (the DAW is being fed bzero silence right now).
//   RESYNC_REQUEST         app -> driver: the app stores a nonce here; the
//                          driver re-anchors in GetZeroTimeStamp and echoes the
//                          nonce back so the app knows the request landed.
#define JB_OFF_SLAVE_PORTS_CONNECTED (0x1b0)
#define JB_OFF_DAEMON_XRUNS          (0x1b8)
#define JB_OFF_DRIVER_FAULT          (0x1c0)
#define JB_OFF_RESYNC_REQUEST        (0x1c8)

// Bit definitions for JB_OFF_DRIVER_FAULT.
#define JB_FAULT_DEVICE_NOT_ALIVE   (1u << 0)

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
// netadapter -l, in netjack cycles. jack2 1.9.22 defaults to 2; we leave it
// unset in jackbridge-pi-up, so the default is what runs.
#define JB_NET_LATENCY_CYCLES       2
// netadapter -g, in frames. Set explicitly in jackbridge-pi-up's jack_load.
#define JB_NETADAPTER_RING_FRAMES   512
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

// One-way leg in frames: pi ADC -> Mac HAL, or Mac HAL -> pi DAC. The two
// directions are symmetric (each carries exactly one codec pass), so the
// monitoring round trip is 2x this.
static inline uint32_t jb_one_way_latency_frames(uint64_t period_frames, uint64_t sample_rate) {
    if (!jb_timing_is_plausible(period_frames, sample_rate)) {
        period_frames = JB_REFERENCE_PERIOD_FRAMES;
        sample_rate   = JB_REFERENCE_SAMPLE_RATE;
    }
    // T_alsa + T_pj + T_l + T_nm + T_mj, all integer multiples of the period.
    const uint64_t period_terms =
        (uint64_t)(JB_ALSA_PERIODS_PI + JB_NET_LATENCY_CYCLES + 3) * period_frames;
    // T_wire, rounded to the nearest frame at this sample rate.
    const uint64_t wire =
        (sample_rate * JB_WIRE_TRANSIT_MICROS + 500000ULL) / 1000000ULL;
    return (uint32_t)(JB_CODEC_GROUP_DELAY_FRAMES +
                      period_terms +
                      (JB_NETADAPTER_RING_FRAMES / 2) +
                      wire);
}

// Monitoring trip: pi ADC -> Mac -> pi DAC. What a guitarist monitoring
// through the Mac actually hears, excluding the DAW's own buffers.
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
static_assert(JB_OFF_DEVICE_NAME >= JB_OFF_RESYNC_REQUEST + 8,
              "device name overlaps the control atomics");
static_assert(JB_OFF_DEVICE_NAME + JB_DEVICE_NAME_MAX <= STRBUF_U0,
              "device name runs into the first ring buffer");
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
    int        FramesPerBuffer;
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
    std::atomic<uint64_t> *shmDriverFault;         // driver writes
    std::atomic<uint64_t> *shmResyncRequest;       // app writes, driver echoes
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
