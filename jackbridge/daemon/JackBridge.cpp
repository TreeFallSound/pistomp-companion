/*
 File: JackBridge.h

MIT License

Copyright (c) 2016-2018 Shunji Uno <madhatter68@linux-dtm.ivory.ne.jp>

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

#include <iostream>
#include <unistd.h>
#include <stdlib.h>
#include <string>
#include <sstream>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <cerrno>
#include <pthread.h>
#include <atomic>
#include "jackClient.hpp"
#include "JackBridge.h"
#include "jb_log.hpp"
#include "workgroup.hpp"
#include "RingProjector.hpp"
#include "RingCopy.hpp"
#include <mach/mach_time.h>
// Minimal config.plist value extraction. Runs once at daemon startup (before
// jack_activate), so blocking file I/O here is fine. We avoid PlistBuddy /
// CFPreferences so the daemon stays a lean C++ JACK client with no CoreFoundation
// dependency. Returns the string value for `key`, or "" when absent/unreadable.
static std::string config_plist_string(const char* key) {
    const char* path = getenv("JACKBRIDGE_CONFIG_PATH");
    if (!path || !*path) return "";
    FILE* f = fopen(path, "r");
    if (!f) return "";
    char line[512];
    std::string result;
    bool want_value = false;
    while (fgets(line, sizeof(line), f)) {
        if (!want_value) {
            char needle[128];
            snprintf(needle, sizeof(needle), "<key>%s</key>", key);
            if (strstr(line, needle)) want_value = true;
            continue;
        }
        // Expect <string>value</string> or <integer>value</integer>.
        static const struct { const char* open; const char* close; size_t len; } tags[] = {
            { "<string>",  "</string>",  8 },
            { "<integer>", "</integer>", 9 },
        };
        for (const auto& t : tags) {
            const char* open = strstr(line, t.open);
            const char* close = open ? strstr(open + t.len, t.close) : nullptr;
            if (open && close && close > open + t.len) {
                result.assign(open + t.len, close);
                break;
            }
        }
        break;
    }
    fclose(f);
    return result;
}

// Numeric config value, clamped. Anything unparseable falls back to `dflt`
// rather than failing the daemon: config.plist is user-editable.
static long config_plist_long(const char* key, long dflt, long lo, long hi) {
    std::string raw = config_plist_string(key);
    if (raw.empty()) return dflt;
    errno = 0;
    char* end = nullptr;
    long v = strtol(raw.c_str(), &end, 10);
    if (errno || !end || *end != '\0' || v < lo || v > hi) return dflt;
    return v;
}

// Which CoreAudio workgroup the JACK realtime thread belongs to. A thread can
// hold exactly one: os_workgroup_join returns EALREADY for a workgroup that
// does not nest with the one it already has.
//
//   backend  jackd's clock device (the audio interface). The deadline the JACK
//            graph actually enforces, and what JackEngine::XRun reports on.
//   hal      the JackBridge virtual device, whose timeline this thread itself
//            publishes via shm.
//   none     plain Mach time-constraint realtime, no workgroup.
enum class WorkgroupMode { Backend, Hal, None };

static WorkgroupMode config_workgroup_mode() {
    std::string v = config_plist_string("Workgroup");
    if (v == "hal")  return WorkgroupMode::Hal;
    if (v == "none") return WorkgroupMode::None;
    return WorkgroupMode::Backend;
}

static const char* workgroup_mode_name(WorkgroupMode m) {
    switch (m) {
        case WorkgroupMode::Hal:  return "hal";
        case WorkgroupMode::None: return "none";
        default:                  return "backend";
    }
}

static WorkgroupMode g_workgroup_mode = WorkgroupMode::Backend;

// Device display name: "pi-Stomp (<host>)" built from DeviceName (explicit
// override) or PiHostname. Bounded to JB_DEVICE_NAME_MAX-1 so the shm field is
// always NUL-terminated even on adversarial config.
static std::string device_display_name() {
    std::string host = config_plist_string("PiHostname");
    if (host.empty()) host = "pistomp.local";
    // Keep the mDNS suffix. The bare label and the .local name are not
    // interchangeable: a pi on both a direct cable and the LAN answers
    // "pistomp.local" on the link-local cable address and "pistomp" on the
    // LAN one. Naming the device "pi-Stomp (pistomp)" pointed at the address
    // the audio link does *not* use. The name is the one we actually connect
    // to, verbatim.
    std::string name = config_plist_string("DeviceName");
    if (name.empty()) name = JB_DEVICE_NAME_FALLBACK;
    std::string display = name + " (" + host + ")";
    if (display.size() >= JB_DEVICE_NAME_MAX) display.resize(JB_DEVICE_NAME_MAX - 1);
    return display;
}

// Set in main() before jack_activate; read by the port-registration callback to
// wake the main thread out of sigwait when slave ports come or go. Notification
// callbacks are forbidden from calling jack_connect (JACK aborts with
// "Cannot callback the server in notification thread"), so we defer the wiring
// pass to the main thread via SIGUSR1.
static pthread_t g_main_thread;
static std::atomic<bool> g_wire_dirty{false};

// Frames the daemon stays ahead of the HAL's read head. Published to shm so
// the HAL reports the same number as SafetyOffset; see JB_OFF_JITTER_FRAMES.
static constexpr long kDefaultJitterFrames = JB_JITTER_FRAMES;
static constexpr long kMaxJitterFrames = 2048;
static long g_jitter_frames = kDefaultJitterFrames;
// Margin past the natural worst case of the daemon-vs-anchor delta before a
// window counts as stalled. The delta sawtooths 0..N every HAL cycle (the
// anchor moves once per N frames while FrameNumber advances every P), so the
// threshold the health check actually applies is N + this margin — a value
// the sawtooth can never reach on a live HAL, but the smallest stall clears.
// A fixed value here cried wolf at every N >= 512.
static constexpr int64_t kSnapThresholdFrames = 512;

// The netadapter loop pair the pi is running, read from config.plist and
// published to shm so the HAL's latency model uses the live values. The Mac
// owns both: jackbridge-ctl pushes them into /etc/default/jackbridge before
// each pi service start. Bounds mirror the clamps in jackbridge-pi-up.
static long g_net_latency_cycles = JB_NET_LATENCY_CYCLES;
static long g_net_ring_frames    = JB_NETADAPTER_RING_FRAMES;

// Automatic re-anchor. The snap corrects drift up to kSnapThresholdFrames and
// nothing corrected anything above it: on 2026-08-30 the daemon snapped 52
// times in one window against a deficit of 484136464 frames (2.8 hours) and
// never converged, while every status field read healthy. Only a fresh daemon
// process re-anchored, which costs the user the DAW device re-select.
//
// So: a deficit this far past the snap threshold is not drift, and snapping at
// it is futile. Hold it for kReanchorWindows consecutive ~5 s windows — long
// enough that a stopped-then-resumed HAL (which _HW_StartIO re-anchors on its
// own) does not trip it — then re-anchor in place.
static constexpr uint64_t kReanchorThresholdFrames = 8 * (uint64_t)kSnapThresholdFrames;
static constexpr unsigned kReanchorWindows = 3;
#ifdef _WITH_MIDI_BRIDGE_
#include <rtmidi/RtMidi.h>
#define MAX_MIDI_PORTS 256
#endif // _WITH_MIDI_BRIDGE_

/*
 * JackBridge.cpp
 */
#define NUM_INPUT_CHANNELS  (NUM_INPUT_STREAMS*2)
#define NUM_OUTPUT_CHANNELS (NUM_OUTPUT_STREAMS*2)

// Must match kDeviceUID in driver/JackBridge/Plug-In/SA_Device.h. Used to
// locate our HAL device for the workgroup-join handshake.
#define JACKBRIDGE_DEVICE_UID "JackBridgeDeviceUID"

class JackBridge : public JackClient, public JackBridgeDriverIF {
public:
    JackBridge(const char* name, int id, int num_Min, int num_Mout) : JackClient(name, JACK_PROCESS_CALLBACK | JACK_XRUN_CALLBACK), JackBridgeDriverIF(id) {
        if (attach_shm() < 0) {
            JB_LOG_ERR(jb_log_shm(), "attach_shm failed (id=%d)", id);
            exit(1);
        }
        // Publish the CoreAudio device name before check_protocol_version() —
        // the HAL reads it in _HW_Open after its own attach+handshake, and it
        // must be present even if the version check later fails and we exit.
        {
            const std::string display = device_display_name();
            memset(*shmDeviceName, 0, JB_DEVICE_NAME_MAX);
            memcpy(*shmDeviceName, display.c_str(), display.size());
            JB_LOG_INFO(jb_log_daemon(), "device name: %{public}s", display.c_str());
        }

        if (!check_protocol_version()) {
            JB_LOG_ERR(jb_log_shm(),
                "shm protocol version mismatch — driver published %llu, daemon built for %d. Reinstall the matching .pkg.",
                (unsigned long long)shmProtocolVersion->load(std::memory_order_acquire),
                JACKBRIDGE_PROTOCOL_VERSION);
            exit(1);
        }

        check_jack_backend();

        isActive = false;
        mLastDriverStatus = JB_DRV_STATUS_ACTIVE;
        isSyncMode = true; // FIXME: should be parameterized
        isVerbose = (getenv("JACKBRIDGE_DEBUG")) ? true : false;
        FrameNumber = 0;
        RingFrames = STRBUFNUM/2;
        shmBufferSize->store(STRBUFSZ, std::memory_order_release);
        shmSyncMode->store(0, std::memory_order_release);

        // Publish the JACK timing the HAL needs for its advertised-latency
        // model. Both are discovered from the Pi at startup (the coordinator
        // passes them to jackd-launch), so the HAL cannot hardcode them.
        shmJackPeriodFrames->store((uint64_t)JackPeriodFrames, std::memory_order_relaxed);
        shmJackSampleRate->store((uint64_t)SampleRate, std::memory_order_release);

        // Protocol-8 self-healing fields the daemon owns. Start from a known
        // zero so the app never reads a stale count out of a reused region.
        shmSlavePortsConnected->store(0, std::memory_order_relaxed);
        shmDaemonXRuns->store(0, std::memory_order_relaxed);
        shmJitterFrames->store((uint64_t)g_jitter_frames, std::memory_order_release);

        // Protocol-10. The live netadapter pair the HAL's latency model needs,
        // and a zeroed health window so a reused region cannot show a stale
        // deficit from the daemon we replaced.
        shmNetLatencyCycles->store((uint64_t)g_net_latency_cycles, std::memory_order_relaxed);
        shmNetRingFrames->store((uint64_t)g_net_ring_frames, std::memory_order_release);
        shmHealthDeltaMax->store(0, std::memory_order_relaxed);
        shmHealthSnaps->store(0, std::memory_order_relaxed);
        shmReanchorCount->store(0, std::memory_order_relaxed);
        // The daemon owns only the geometry bit in the shared fault word.
        shmDriverFault->fetch_and(~(uint64_t)JB_FAULT_BAD_RING_GEOMETRY,
                                  std::memory_order_release);
        // Cadence counters, same treatment: a reused region must not show
        // the previous daemon's counts.
        shmDupReadCycles->store(0, std::memory_order_relaxed);
        shmSkipReadFrames->store(0, std::memory_order_relaxed);
        shmDupWriteCycles->store(0, std::memory_order_relaxed);
        shmSkipWriteFrames->store(0, std::memory_order_relaxed);
        shmRecvResyncs->store(0, std::memory_order_relaxed);
        // Take the app's current nonce as already-seen: a nonce left in a
        // region from a previous run is not a request aimed at us.
        mLastResyncRequest = shmResyncRequest->load(std::memory_order_acquire);

        config_audio_ports();
#ifdef _WITH_MIDI_BRIDGE_
        create_midi_ports(name, num_Min, num_Mout);
        register_ports((const char**)nameAin, (const char**)nameAout, (const char**)nameMin, (const char**)nameMout);
#else
        register_ports((const char**)nameAin, (const char**)nameAout, NULL, NULL);
#endif // _WITH_MIDI_BRIDGE_

        // Must be set before jack_activate (in JackClient::activate). Fires for
        // every port registration after activation — we filter for slave ports
        // and re-run auto_wire() so connections survive netmanager reloads or
        // pi restarts.
        jack_set_port_registration_callback(client, _port_registration_callback, this);

        // Departure handling. A slave that goes away — cleanly or by a yanked
        // cable — surfaces here as a port disconnect or deregistration. We
        // re-run the wiring pass (harmless if nothing changed) and, more
        // importantly, recount how many of our slave ports still have a live
        // connection so the app can tell "streaming" from "feeding the DAW
        // silence against a corpse".
        jack_set_port_connect_callback(client, _port_connect_callback, this);

        lastTraceFrame = 0;

        // Best-effort workgroup acquisition. May fail if Core Audio hasn't
        // surfaced our HAL device yet (e.g. coreaudiod is mid-rescan) — the
        // process callback retries until it succeeds. Joining the workgroup
        // happens lazily on the JACK RT thread because os_workgroup_join must
        // run on the joining thread itself.
        mWorkgroup = (g_workgroup_mode == WorkgroupMode::Hal)
            ? workgroup_acquire_by_uid(JACKBRIDGE_DEVICE_UID)
            : NULL;
        mWorkgroupJoined = (g_workgroup_mode != WorkgroupMode::Hal);
        mWorkgroupAcquireBackoff = 0;

        JB_LOG_INFO(jb_log_daemon(),
            "JackBridge#%u: start sr=%d Hz, bufsize=%u bytes, jitter=%ld frames",
            instance, SampleRate, (unsigned)JackPeriodFrames, g_jitter_frames);
        const uint32_t oneWay = jb_one_way_latency_frames(
            JackPeriodFrames, SampleRate, (uint64_t)g_net_latency_cycles, (uint64_t)g_net_ring_frames);
        JB_LOG_INFO(jb_log_daemon(),
            "latency model: period=%u f_s=%d L=%ld G=%ld -> one-way=%u frames, monitoring trip=%u frames (%.1f ms)",
            (unsigned)JackPeriodFrames, SampleRate, g_net_latency_cycles, g_net_ring_frames,
            oneWay, 2 * oneWay,
            SampleRate > 0 ? 1000.0 * 2 * oneWay / SampleRate : 0.0);
    }

    ~JackBridge() {
        // Skip os_workgroup_leave: jack_client_close (in ~JackClient) tears
        // down the RT thread, and the kernel releases workgroup membership
        // when the thread dies. Just drop our reference.
        if (mWorkgroup) {
            os_release(mWorkgroup);
            mWorkgroup = NULL;
        }
#ifdef _WITH_MIDI_BRIDGE_
        release_midi_ports();
#endif // _WITH_MIDI_BRIDGE_
    }

    // Refuse to start if jackd's backend is anything other than CoreAudio,
    // or if jackd's CoreAudio backend is pointed at JackBridge itself
    // (clock-device feedback loop).
    void check_jack_backend() {
        jack_port_t* port = jack_port_by_name(client, "system:playback_1");
        if (!port) {
            JB_LOG_ERR(jb_log_jack(),
                "no system:playback_1 port — jackd has no backend or backend has no playback. "
                "JackBridge requires a CoreAudio backend (-d coreaudio). See docs/macos-setup.md.");
            exit(1);
        }

        size_t alias_size = jack_port_name_size();
        char* alias_storage[2] = {
            (char*)calloc(1, alias_size),
            (char*)calloc(1, alias_size),
        };
        int n = jack_port_get_aliases(port, alias_storage);

        if (n <= 0) {
            JB_LOG_ERR(jb_log_jack(),
                "system:playback_1 has no aliases — jackd backend is likely 'net' or a "
                "non-CoreAudio driver. Required: coreaudio. See docs/macos-setup.md.");
            free(alias_storage[0]);
            free(alias_storage[1]);
            exit(1);
        }

        // Feedback-loop check: if jackd's clock device is JackBridge itself
        // (directly or via an aggregate whose name contains "JackBridge"),
        // CoreAudio doesn't detect the cycle — output is silence or runaway.
        // The HAL device's display name is "JackBridge" (see Localizable.strings).
        for (int i = 0; i < n; i++) {
            if (strstr(alias_storage[i], "JackBridge") != NULL) {
                JB_LOG_ERR(jb_log_jack(),
                    "jackd is clocked off JackBridge itself (alias=%{public}s). "
                    "This creates a CoreAudio feedback loop. Set ClockDeviceUID in "
                    "JackBridge Settings to a different device (e.g. built-in output). "
                    "See docs/idiosyncrasies.md.",
                    alias_storage[i]);
                free(alias_storage[0]);
                free(alias_storage[1]);
                exit(1);
            }
        }

        JB_LOG_INFO(jb_log_jack(), "backend check OK (alias=%{public}s)", alias_storage[0]);
        free(alias_storage[0]);
        free(alias_storage[1]);
    }

    int process_callback(jack_nframes_t nframes) override {
        sample_t *ain[NUM_INPUT_CHANNELS];
        sample_t *aout[NUM_OUTPUT_CHANNELS];

        // First-call workgroup wiring. Per WWDC20 "Meet Audio Workgroups",
        // joining the device's IO-thread workgroup tells the kernel scheduler
        // to treat this thread as co-deadline with the HAL's IOProc — exactly
        // the relationship that exists across our shm bridge. Without it
        // we've seen the Mac scheduler hand the IOProc multiple cycles of
        // backlog at once, manifesting as the "guarantee MISS" log lines on
        // the driver side. Join is one-shot, retried until acquisition
        // succeeds (Core Audio may not have surfaced the device at startup).
        if (!mWorkgroupJoined) {
            if (!mWorkgroup) {
                // Backoff: ~once per ~1s at 48k/64 (~750 cycles).
                if (++mWorkgroupAcquireBackoff >= 750) {
                    mWorkgroupAcquireBackoff = 0;
                    // CoreAudio property calls on the RT thread. Only the
                    // startup acquire in the constructor normally runs; this
                    // is the fallback for a device that was not up yet.
                    mWorkgroup = workgroup_acquire_by_uid(JACKBRIDGE_DEVICE_UID);
                }
            }
            if (mWorkgroup) {
                uint64_t period_ns =
                    (uint64_t)1000000000ULL * (uint64_t)nframes / (uint64_t)SampleRate;
                uint64_t computation_ns = period_ns / 2;
                if (workgroup_join_self(mWorkgroup, &mWorkgroupJoinToken,
                                        period_ns, computation_ns) == 0) {
                    mWorkgroupJoined = true;
                } else {
                    // Drop the workgroup so we don't tight-loop on join failure.
                    os_release(mWorkgroup);
                    mWorkgroup = NULL;
                }
            }
        }

        // Heartbeat — HAL watches this counter; if it stops advancing the HAL
        // feeds the DAW silence and raises the shm fault bit. The HAL no longer
        // takes the device down with it (see SA_Device.cpp
        // kAudioDevicePropertyDeviceIsAlive), so a stall costs silence, not a
        // manual device re-selection. relaxed is fine: the staleness check only
        // cares that the value moves, not what the value is.
        shmDaemonAlive->fetch_add(1, std::memory_order_relaxed);

#ifdef _WITH_MIDI_BRIDGE_
        process_midi_message(nframes);
#endif // _WITH_MIDI_BRIDGE_

        // State machine: detect HAL restart BEFORE the early-return path.
        // When the DAW stops IO, _HW_StopIO sets shmDriverStatus = ACTIVE.
        // The daemon then returns early (below) WITHOUT advancing FrameNumber.
        // When IO resumes, _HW_StartIO sets shmDriverStatus = STARTED. We
        // must detect that transition here, before we return early again,
        // or mLastDriverStatus stays stale forever.
        uint64_t currentStatus = shmDriverStatus->load(std::memory_order_acquire);
        if (currentStatus == JB_DRV_STATUS_STARTED &&
            mLastDriverStatus != JB_DRV_STATUS_STARTED) {
            reanchor("HAL restart", (int)nframes);
        }
        mLastDriverStatus = currentStatus;

        // Requested re-anchor. RESYNC_REQUEST is the app's field (the menu
        // bar writes a nonce into it); the driver honours it for its own
        // liveness state, and we honour it for the timeline — which is the
        // half that matters, because syncMode is 1 and the daemon owns the
        // anchor. Compare against the last value seen so one write fires
        // exactly once even though the nonce stays visible forever.
        {
            uint64_t resync = shmResyncRequest->load(std::memory_order_acquire);
            if (resync != mLastResyncRequest) {
                mLastResyncRequest = resync;
                if (resync != 0) reanchor("resync request", (int)nframes);
            }
        }

        if (currentStatus != JB_DRV_STATUS_STARTED) {
            // Driver isn't working. Just return zero buffer;
            for(int i=0; i<NUM_OUTPUT_CHANNELS; i++) {
                aout[i] = (sample_t*)jack_port_get_buffer(audioOut[i], nframes);
                bzero(aout[i], STRBUFSZ);
            }
            return 0;
        }

        // For DEBUG
        check_progress((int)nframes);

        if (!isActive) {
            ncalls = 0;
            FrameNumber = 0;

            if (isSyncMode) {
                shmSyncMode->store(1, std::memory_order_relaxed);
                shmNumberTimeStamps->store(0, std::memory_order_relaxed);
                shmSeed->fetch_add(1, std::memory_order_release);
            }
            // Seed the upstream read cursor on the first active cycle: the
            // target depends on the HAL heads, which read 0 before the HAL
            // has run a cycle.
            mRecvCursor = mCachedHalOutputWriteHead
                        - block_clearance((int)nframes) - (uint64_t)g_jitter_frames;
            mRecvCursorSeeded = true;

            // Seed the send cursor beside the recv cursor: its target
            // depends on the HAL's read head, which reads 0 before the HAL
            // has run a cycle. Planted at the send target of the CURRENT
            // head, so the walk starts aligned and the head's next jump is
            // a full group away — the steady regime the snap rule was
            // derived for.
            mSendCursor = mCachedHalInputReadHead
                        + block_clearance((int)nframes) + (uint64_t)g_jitter_frames;
            mSendCursorSeeded = true;

            isActive = true;
            // FIXME(rt-safety): os_log on the JACK process callback path is
            // not strictly RT-safe (may take internal locks). Fires once per
            // activation, so the practical cost is bounded — revisit if it
            // shows up under load.
            JB_LOG_INFO(jb_log_jack(),
                "JackBridge#%u: activated SyncMode=%{public}s ZeroHostTime=0x%llx",
                instance, isSyncMode ? "yes" : "no",
                (unsigned long long)shmZeroHostTime->load(std::memory_order_acquire));
        }

        // Per-lap timeline refresh (syncMode 1): the driver relays
        // ZeroHostTime/NumberTimeStamps to build its timestamps on every IO
        // cycle (SA_Device.cpp GetZeroTimeStamp), so these two fields are the
        // live timeline, not a one-shot. Republishing at each lap boundary
        // bounds how far a JACK xrun (which drops FrameNumber but not the
        // wall clock) can offset the HAL's extrapolated timeline from the
        // daemon's: without this refresh the offset persists until the next
        // re-anchor. Relaxed store for the host time, release store for the
        // sample-time count so a reader never sees the new count with the
        // old anchor.
        if ((FrameNumber % RingFrames) == 0) {
            shmZeroHostTime->store(mach_absolute_time(),
                                   std::memory_order_relaxed);
            shmNumberTimeStamps->store(FrameNumber / RingFrames,
                                       std::memory_order_release);
        }

        if ((!isSyncMode) && isVerbose && ((ncalls++) % 100) == 0) {
            uint64_t zht = shmZeroHostTime->load(std::memory_order_acquire);
            printf("JackBridge#%d: ZeroHostTime: %llx, %llu, diff:%d\n",
                instance, zht,
                shmNumberTimeStamps->load(std::memory_order_acquire),
                ((int)(mach_absolute_time()+1000000-zht))-1000000);
        }

        for(int i=0; i<NUM_INPUT_CHANNELS; i++) {
            ain[i] = (sample_t*)jack_port_get_buffer(audioIn[i], nframes);
        }
        sendToCoreAudio(ain, nframes);


        for(int i=0; i<NUM_OUTPUT_CHANNELS; i++) {
            aout[i] = (sample_t*)jack_port_get_buffer(audioOut[i], nframes);
        }
        receiveFromCoreAudio(aout, nframes);

        FrameNumber += nframes;

        return 0;
    }

    // jackd reports an xrun whenever a process cycle overruns its period or a
    // backend cycle is dropped (netJACK2 packet loss surfaces here too). The
    // callback signature gives no frame count, so we just count and let
    // check_progress() roll it into the 5s drift trace — RT-safer than logging
    // per event.
    int xrun_callback() override {
        mXRunCount.fetch_add(1, std::memory_order_relaxed);
        // Monotonic mirror for the app. mXRunCount is drained every 5s by
        // check_progress(), so it can't be published directly. This one only
        // ever climbs; the app watches its *rate* — a fast ramp is netmanager
        // stalling ~2s per cycle against a dead pi, i.e. "no audio from pi".
        shmDaemonXRuns->fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    void setVerbose(bool flag) {
        JB_LOG_INFO(jb_log_daemon(),
            "JackBridge#%u: verbose mode %{public}s", instance, flag ? "on" : "off");
        isVerbose = flag;
    }

    // Auto-wire netmanager slave ports to our HAL bridge ports. Called once
    // after activate() to pick up slaves that registered before us, and again
    // from the port-registration callback when a new slave shows up later
    // (netadapter reload, pi reboot, etc.). Idempotent: jack_connect returns
    // EEXIST for already-connected pairs, which we treat as success.
    //
    // Policy is "first match per channel": the first *:from_slave_<n> seen is
    // wired to JackBridge input_<n>, etc. The graph contract is fixed.
    void auto_wire() {
        wire_direction("from_slave", JackPortIsOutput, audioIn, nAudioIn);
        wire_direction("to_slave",   JackPortIsInput,  audioOut, nAudioOut);
    }

    void on_shutdown() override {
        // Called by jackd when it goes away (intentional stop, crash, whatever).
        // Zero the heartbeat so the HAL starts feeding silence immediately
        // rather than after the 5-cycle threshold, then nudge main()'s sigwait
        // so we exit cleanly. LaunchAgent KeepAlive brings us back when jackd
        // is back.
        //
        // Exiting here costs a restart gap the user hears. A cable fault no
        // longer reaches this path -- jackd survives one since the CoreAudio
        // workgroup and SIGPIPE fixes in the jack2 fork (docs/idiosyncrasies.md,
        // "jackd on macOS") -- so what remains here is a genuine jackd stop.
        //
        // We write the heartbeat and NOT shmDriverStatus. That field belongs
        // to the driver, and writing INIT here was a self-inflicted deadlock:
        // nothing but _HW_StartIO ever writes STARTED back, so the daemon that
        // LaunchAgent started next read INIT, took its own "driver isn't
        // working" early return in process(), and zeroed the output buffers
        // for good -- 6/6 ports wired, packets flowing, and silence, until the
        // user re-selected the device in the DAW. That was the whole of the
        // post-replug failure. The driver now also re-asserts the field on
        // every GetZeroTimeStamp, so an older daemon cannot wedge a newer
        // driver either.
        shmDaemonAlive->store(0, std::memory_order_release);
        JB_LOG_DEFAULT(jb_log_jack(), "jackd shut down — exiting for LaunchAgent restart");
        kill(getpid(), SIGTERM);
    }

    static void _port_registration_callback(jack_port_id_t port_id, int registered, void* arg) {
        JackBridge* self = (JackBridge*)arg;
        jack_port_t* port = jack_port_by_id(self->client, port_id);
        if (!port) {
            // A deregistration can race the lookup. Still nudge the main
            // thread so it recounts live connections.
            self->mark_wire_dirty();
            return;
        }
        const char* shortname = jack_port_short_name(port);
        if (!shortname) return;
        if (strncmp(shortname, "from_slave_", 11) == 0 ||
            strncmp(shortname, "to_slave_", 9)   == 0) {
            // React to departures too now: on a deregistration the main thread
            // re-runs auto_wire (a no-op) and, crucially, recounts how many of
            // our slave ports still carry a live connection. jack_connect and
            // jack_port_connected are both illegal from a notification thread,
            // so defer via SIGUSR1.
            (void)registered;
            self->mark_wire_dirty();
        }
    }

    static void _port_connect_callback(jack_port_id_t a, jack_port_id_t b,
                                       int connect, void* arg) {
        (void)a; (void)b; (void)connect;
        // Any connect/disconnect anywhere in the graph is cheap to respond to:
        // just recount our own ports on the main thread.
        ((JackBridge*)arg)->mark_wire_dirty();
    }

    void mark_wire_dirty() {
        g_wire_dirty.store(true, std::memory_order_release);
        pthread_kill(g_main_thread, SIGUSR1);
    }

    // Recount how many of our slave-facing ports currently have at least one
    // connection, and publish the total (0..NUM_INPUT_CHANNELS+NUM_OUTPUT_CHANNELS)
    // into shm. Main thread only — jack_port_connected is not RT-safe.
    //
    // This is a structural signal: it drops to 0 the moment jackd reaps a
    // departed slave's ports. The fork's reaping fixes (Phase 2) are what make
    // that reap prompt; until then the app leans on the xrun rate.
    void publish_slave_health() {
        int connected = 0;
        for (int i = 0; i < nAudioIn; i++)
            if (audioIn[i] && jack_port_connected(audioIn[i]) > 0) connected++;
        for (int i = 0; i < nAudioOut; i++)
            if (audioOut[i] && jack_port_connected(audioOut[i]) > 0) connected++;
        shmSlavePortsConnected->store((uint64_t)connected, std::memory_order_release);
    }

private:
    // Walks all ports of the given direction, filters by short-name == "<role>_<n>",
    // and connects the first match per channel to our local port `local[n-1]`.
    // `flags` selects the *remote* port direction: JackPortIsOutput when we're
    // looking for sources to feed our inputs, JackPortIsInput when we're looking
    // for sinks for our outputs.
    void wire_direction(const char* role, unsigned long flags,
                        jack_port_t* const* local, int n_local) {
        const char** ports = jack_get_ports(client, NULL,
                                            JACK_DEFAULT_AUDIO_TYPE, flags);
        if (!ports) return;

        for (int ch = 1; ch <= n_local; ch++) {
            char suffix[32];
            snprintf(suffix, sizeof(suffix), "%s_%d", role, ch);

            const char* match = NULL;
            for (const char** p = ports; *p; p++) {
                const char* colon = strrchr(*p, ':');
                if (!colon) continue;
                if (strcmp(colon + 1, suffix) != 0) continue;
                match = *p;
                break;
            }
            if (!match) continue;

            // jack_connect takes source first, then destination.
            const char* local_name = jack_port_name(local[ch - 1]);
            const char* src = (flags & JackPortIsOutput) ? match      : local_name;
            const char* dst = (flags & JackPortIsOutput) ? local_name : match;

            int rc = jack_connect(client, src, dst);
            if (rc == 0) {
                JB_LOG_INFO(jb_log_jack(),
                    "auto-wire: %{public}s -> %{public}s", src, dst);
            } else if (rc != EEXIST) {
                JB_LOG_DEFAULT(jb_log_jack(),
                    "auto-wire: jack_connect %{public}s -> %{public}s failed rc=%d",
                    src, dst, rc);
            }
        }

        jack_free(ports);
    }

    bool isActive, isSyncMode, isVerbose;
    uint64_t lastTraceFrame;
    int64_t ncalls;
    uint64_t mLastDriverStatus;
    char** nameAin;
    char** nameAout;

    // Workgroup wiring; see process_callback for the lazy-join rationale.
    os_workgroup_t mWorkgroup;
    os_workgroup_join_token_s mWorkgroupJoinToken;
    bool mWorkgroupJoined;
    int  mWorkgroupAcquireBackoff;

    // RT-safe event counters drained by check_progress() every 5s.
    std::atomic<uint32_t> mXRunCount{0};

    // Health-window accumulators, single-writer (JACK process thread).
    // deltaMax: peak stall depth (FrameNumber - mLastHalProgressFrame) in the
    // window. Zero in steady state; grows when the HAL IOProc stalls and stops
    // advancing its per-operation anchor. Snap fires when a stall exceeds
    // kSnapThresholdFrames.
    uint64_t mHealthDeltaMax{0};
    std::atomic<uint32_t> mSnapCount{0};
    // Consecutive health windows whose deltaMax stayed past
    // kReanchorThresholdFrames. Reaching kReanchorWindows re-anchors.
    unsigned mDivergedWindows{0};
    // Last JB_OFF_RESYNC_REQUEST nonce we acted on.
    uint64_t mLastResyncRequest{0};

    // HAL head cache — written by check_progress() every JACK cycle (before
    // send/recv), read by sendToCoreAudio/receiveFromCoreAudio in the same
    // cycle. Single-writer (JACK process thread), no atomic needed.
    uint64_t mCachedHalInputReadHead{0};   // HAL mInputTime.mSampleTime
    uint64_t mCachedHalOutputWriteHead{0}; // HAL mOutputTime.mSampleTime
    // The daemon's downstream write position, in absolute frames — the
    // mirror of mRecvCursor. The old code reconstructed this position every
    // cycle from the HAL's read head plus an open-loop delta term
    // (mLastSyncedReadFrame, now deleted); the cursor accumulates that walk
    // itself: advanced by nframes once per JACK cycle in sendToCoreAudio(),
    // written at `mSendCursor & (RingFrames - 1)`, and snapped to
    // RingProjector::send_target() only when it leaves the safe window (see
    // check_progress). Seeded on the first active cycle and re-seeded in
    // reanchor(). A stalled consumer is absorbed open-loop — the same
    // behaviour the delta term existed for — so only the two hard hazards
    // snap: a write that would tear into the consumer's live block, or one
    // that would wrap a full ring onto it.
    uint64_t mSendCursor{0};
    bool     mSendCursorSeeded{false};
    // Send-side snap count, published on the 5s os_log health line. It needs
    // a shm slot in the next protocol bump; ShmReader and the runbook must be
    // updated with it at the same time. Until then, the audio cost is visible
    // in dupWrite/skipWrite (event count for backward snaps, frames for skips).
    uint64_t mSendResyncs{0};

    // The daemon's upstream read position, in absolute frames — the same
    // domain as FrameNumber and the HAL heads, because syncMode is 1 and
    // both sides share the CoreAudio clock. Advanced by nframes once per
    // JACK cycle in receiveFromCoreAudio(), read at
    // `mRecvCursor & (RingFrames - 1)`, and snapped to
    // RingProjector::recv_target() only when it leaves the safe window
    // (see check_progress). Seeded on the first active cycle and re-seeded
    // in reanchor(): it is a timeline position, so it moves with the anchor.
    uint64_t mRecvCursor{0};
    bool     mRecvCursorSeeded{false};

    // Liveness, kept deliberately separate from the ring pair above.
    // mCachedHalAnchorSample is mCurrentTime.mSampleTime, which the HAL writes
    // on EVERY IO operation; the two heads carry a valid timestamp only on the
    // operation that owns their direction. A playback-only client issues
    // nothing but WriteMix, so halInputReadHead never advances even though the
    // HAL is running every cycle. Keying the health check on the read head
    // therefore reported a healthy HAL as a permanent stall and re-anchored
    // roughly every 15 s. The anchor advances whenever the IOProc runs, in
    // either direction, so it answers "is the HAL alive" without any claim
    // about ring positions.
    uint64_t mCachedHalAnchorSample{0};
    // FrameNumber at the last HAL anchor advance — the health delta's
    // reference point (see check_progress).
    uint64_t mLastHalProgressFrame{0};
    // The HAL's block size, which is NOT the JACK period: any host can set
    // kAudioDevicePropertyBufferFrameSize anywhere in [32, 2048]
    // (SA_Device.cpp:1069-1081), and nothing reconfigures the ring when it
    // does. The send write must clear the block the HAL is reading, which
    // is this size; the upstream read must clear the block the daemon
    // itself reads, the JACK period. Passing the larger of the two
    // satisfies both. 0 until the HAL has run a cycle.
    uint32_t mCachedHalNFrames{0};
    bool mRingGeometryFaulted{false};

    // Cadence counters and the previous cycle's absolute cursor positions.
    // Single-writer (JACK process thread); the shm copies are the published
    // mirror.
    uint64_t mPrevSendCursor{0};
    uint64_t mPrevRecvCursor{0};
    uint64_t mDupReadCycles{0};
    uint64_t mSkipReadFrames{0};
    uint64_t mDupWriteCycles{0};
    uint64_t mSkipWriteFrames{0};
    uint64_t mRecvResyncs{0};
    bool     mCadencePrimed{false};

    // Frames of clearance the ring projections need this cycle.
    uint32_t block_clearance(int nframes) const {
        const uint32_t mine = (uint32_t)(nframes > 0 ? nframes : 0);
        return mCachedHalNFrames > mine ? mCachedHalNFrames : mine;
    }

    // The one place a RingProjector is built. check_progress() and both copy
    // paths must agree exactly -- the cadence counters are only a measurement
    // of the copy paths while they project from identical inputs.
    RingProjector projector(int nframes) const {
        return RingProjector{
            (uint32_t)RingFrames, block_clearance(nframes), mCachedHalNFrames,
            mCachedHalInputReadHead, mCachedHalOutputWriteHead,
            (uint32_t)(nframes > 0 ? nframes : 0),
            (int32_t)g_jitter_frames
        };
    }

    int sendToCoreAudio(float** in, int nframes) {
        // The downstream write is free-running, mirroring the read side:
        // write at the cursor's ring position, then advance it by one
        // period. check_progress() has already snapped the cursor to its
        // target if it left the safe window, so the position here is always
        // the corrected one.
        const uint32_t offset = mSendCursor & ((uint32_t)RingFrames - 1);
        for (int j = 0; j < NUM_INPUT_STREAMS; j++) {
            ring_write_stereo_interleaved(
                buf_down[j], (uint32_t)RingFrames, offset,
                in[j*2 + 0], in[j*2 + 1], nframes);
        }
        mSendCursor += (uint64_t)nframes;
        return nframes;
    }

    int receiveFromCoreAudio(float** out, int nframes) {
        // The upstream read is free-running: read at the cursor's ring
        // position, then advance it by one period. check_progress() has
        // already snapped the cursor to its target if it left the safe
        // window, so the position here is always the corrected one.
        const uint32_t offset = mRecvCursor & ((uint32_t)RingFrames - 1);
        for (int j = 0; j < NUM_OUTPUT_STREAMS; j++) {
            ring_consume_stereo_interleaved(
                buf_up[j], (uint32_t)RingFrames, offset,
                out[j*2 + 0], out[j*2 + 1], nframes);
        }
        mRecvCursor += (uint64_t)nframes;
        return nframes;
    }

    void config_audio_ports() {
        nameAin = (char**)malloc(sizeof(char*)*(NUM_INPUT_CHANNELS+1));
        for(int i=0; i<NUM_INPUT_CHANNELS; i++) {
            nameAin[i] = (char*)malloc(256);
            snprintf(nameAin[i], 256, "input_%d", i+1);
        }
        nameAin[NUM_INPUT_CHANNELS] = nullptr;

        nameAout = (char**)malloc(sizeof(char*)*(NUM_OUTPUT_CHANNELS+1));
        for(int i=0; i<NUM_OUTPUT_CHANNELS; i++) {
            nameAout[i] = (char*)malloc(256);
            snprintf(nameAout[i], 256, "output_%d", i+1);
        }
        nameAout[NUM_OUTPUT_CHANNELS] = nullptr;
    }

#ifdef _WITH_MIDI_BRIDGE_
    RtMidiOut  **midiout;
    RtMidiIn   **midiin;
    int nOutPorts, nInPorts;
    char** nameMin;
    char** nameMout;

    int get_num_ports(unsigned long flags) {
        int num;
        const char** ports = jack_get_ports(client, "system", ".*raw midi", flags);
        if (!ports) {
            return 0;
        }

        for(num=0;*ports != NULL; ports++,num++) {
#if 0 // For DEBUG
            jack_port_t* p = jack_port_by_name(client, *ports);
            std::cout << ";" << *ports << ";" << jack_port_short_name(p) << ";" << jack_port_type(p) << std::endl;
#endif
        }
        return num;
    }

    void create_midi_ports(const char* name, int num_Min, int num_Mout) {
        char buf[256];

        // create bridge from Jack to CoreMIDI
        nOutPorts = (num_Mout < 0) ? get_num_ports(JackPortIsOutput) : num_Mout;
        midiout = (RtMidiOut**)malloc(sizeof(RtMidiOut*)*nOutPorts);
        nameMin = (char**)malloc(sizeof(char*)*(nOutPorts+1));

        for(int n=0; n<nOutPorts; n++) {
            try {
                midiout[n] = new RtMidiOut(RtMidi::MACOSX_CORE);
                snprintf(buf, 256, "%s %d", name, n+1);
                midiout[n]->openVirtualPort(buf);
            } catch ( RtMidiError &error ) {
                error.printMessage();
                exit( EXIT_FAILURE );
            }

            nameMin[n] = (char*)malloc(256);
            snprintf(nameMin[n], 256, "event_in_%d", n+1);
        }
        nameMin[nOutPorts] = NULL;

        // create bridge from CoreMIDI to Jack
        nInPorts = (num_Min < 0) ? get_num_ports(JackPortIsInput) : num_Min;
        midiin = (RtMidiIn**)malloc(sizeof(RtMidiIn*)*nInPorts);
        nameMout = (char**)malloc(sizeof(char*)*(nInPorts+1));

        for(int n=0; n<nInPorts; n++) {
            try {
                midiin[n] = new RtMidiIn(RtMidi::MACOSX_CORE);
                snprintf(buf, 256, "%s %d", name, n+1);
                midiin[n]->openVirtualPort(buf);
                midiin[n]->ignoreTypes(false, false, false);
            } catch ( RtMidiError &error ) {
                error.printMessage();
                exit( EXIT_FAILURE );
            }

            nameMout[n] = (char*)malloc(256);
            snprintf(nameMout[n], 256, "event_out_%d", n+1);
        }
        nameMout[nInPorts] = NULL;
    }

    void release_midi_ports() {
        // release bridge from Jack to CoreMIDI
        for(int n=0; n<nOutPorts; n++) {
            delete midiout[n];
            free(nameMin[n]);
        }
        free(midiout);
        free(nameMin);

        // release bridge from CoreMIDI to Jack
        for(int n=0; n<nInPorts; n++) {
            delete midiin[n];
            free(nameMout[n]);
        }
        free(midiin);
        free(nameMout);
    }

    void process_midi_message(jack_nframes_t nframes) {
        void *min, *mout;
        int count;
        jack_midi_event_t event;
        std::vector< unsigned char > message;
        jack_midi_data_t* buf;

        // process bridge from Jack to CoreMIDI
        for(int n=0; n<nOutPorts; n++) {
            min = jack_port_get_buffer(midiIn[n], nframes);
            count = jack_midi_get_event_count(min);
            for(int i=0; i<count; i++) {
                jack_midi_event_get(&event, min, i);
                message.clear();
                for (int j=0; j<event.size; j++) {
                    message.push_back(event.buffer[j]);
                }
                if (message.size() > 0) {
                    midiout[n]->sendMessage(&message);
                }
            }
        }

        // process bridge from CoreMIDI to Jack
        for(int n=0; n<nInPorts; n++) {
            mout = jack_port_get_buffer(midiOut[n], nframes);
            jack_midi_clear_buffer(mout);
            midiin[n]->getMessage(&message);
            while(message.size() > 0) {
                buf = jack_midi_event_reserve(mout, 0, message.size());
                if (buf != NULL) {
                    for(int i=0; i<message.size(); i++) {
                        buf[i] = message[i];
                    }
                } else {
                    JB_LOG_ERR(jb_log_jack(), "jack_midi_event_reserve failed");
                }
                midiin[n]->getMessage(&message);
            }
        }
    }
#endif // _WITH_MIDI_BRIDGE_

    // The one re-anchor path: map wall-clock now to the current frozen
    // FrameNumber so the HAL resumes reading where we are writing, and bump
    // the seed so it notices. FrameNumber itself is never reset — that would
    // be a timeline discontinuity and would break transport progression.
    //
    // Every caller reaches the same state a freshly started daemon reaches, so
    // nothing needs a process restart to recover: HAL restart, an app resync
    // request, and the automatic divergence re-anchor all land here.
    //
    // RT-safe: atomic stores and one os_log line, the same shape the
    // HAL-restart branch has always had on this thread.
    void reanchor(const char* reason, int nframes) {
        shmZeroHostTime->store(mach_absolute_time(), std::memory_order_relaxed);
        shmNumberTimeStamps->store(FrameNumber / RingFrames,
                                   std::memory_order_release);
        shmSeed->fetch_add(1, std::memory_order_release);
        // The deficit is measured from the anchor, so it goes with it. The
        // cursors are timeline positions, so they re-seed here too.
        mSendCursor = mCachedHalInputReadHead
                    + block_clearance(nframes) + (uint64_t)g_jitter_frames;
        mRecvCursor = mCachedHalOutputWriteHead
                    - block_clearance(nframes) - (uint64_t)g_jitter_frames;
        mLastHalProgressFrame = FrameNumber;
        mHealthDeltaMax   = 0;
        mDivergedWindows  = 0;
        shmReanchorCount->fetch_add(1, std::memory_order_relaxed);
        JB_LOG_INFO(jb_log_jack(),
            "JackBridge#%u: re-anchored after %{public}s frame=%llu",
            instance, reason, (unsigned long long)FrameNumber);
    }

    // Health trace. Per-cycle: snapshot the HAL's read head, compute the
    // margin (FrameNumber - halReadHead — should sit at JitterFrames), and
    // accumulate integrated deviation and deficit. Emit one line every ~5s.
    //
    // All four values are zero in healthy steady state. Any nonzero value is
    // the only thing worth reading.
    //
    // Tail with:
    //   log stream --predicate 'subsystem == "com.treefallsound.companion" && category == "shm"'
    // nframes comes from the process callback rather than JackPeriodFrames:
    // that member is read once at client open (jackClient.cpp:95) and no
    // buffer-size callback refreshes it, so it is the value that can go stale
    // while the callback's own nframes cannot.
    void check_progress(int nframes) {
        // Snapshot HAL anchor under seqlock. Also captures the output write
        // head, which the old code omitted (it was unused). Single retry loop
        // — the HAL writer is fast (a few atomic stores), so contention is
        // bounded and this is RT-safe.
        uint64_t halReadHead = 0, halWriteHead = 0, halAnchorSample = 0;
        uint64_t halNFrames = 0;
        uint64_t s1, s2;
        do {
            s1 = shmHalAnchorSeq->load(std::memory_order_acquire);
            halReadHead  = shmHalInputReadHead->load(std::memory_order_relaxed);
            halWriteHead = shmHalOutputWriteHead->load(std::memory_order_relaxed);
            // Same snapshot as the heads: the HAL writes all four inside one
            // seqlock bracket, so reading them here costs nothing extra.
            halAnchorSample = shmHalAnchorSampleTime->load(std::memory_order_relaxed);
            halNFrames = shmHalNFrames->load(std::memory_order_relaxed);
            s2 = shmHalAnchorSeq->load(std::memory_order_acquire);
        } while ((s1 & 1) || s1 != s2);

        // Update cached heads before send/recv use them this cycle. The
        // moved-boolean is captured BEFORE the cache write because the snap
        // rules below gate on it: a head that published no new position
        // since last cycle means no IO op ran in that direction (a
        // playback-only session never runs ReadInput; a recording-only
        // session never runs WriteMix), so there is no live block to tear
        // into and the error is measured against a stale target. Snapping
        // against a frozen head produced a false steady snap rate in
        // one-direction sessions — on the recv side this shipped in v12
        // and read as a phantom clock-rate error. The cursor's open-loop
        // walk is correct while the head is frozen; on resume CoreAudio's
        // sample time is host-clock-derived, so the head jumps by exactly
        // the elapsed frames, the walk's error is preserved, and no snap
        // or cost occurs.
        const bool sendHeadMoved = (halReadHead != mCachedHalInputReadHead);
        const bool recvHeadMoved = (halWriteHead != mCachedHalOutputWriteHead);
        if (sendHeadMoved) {
            mCachedHalInputReadHead = halReadHead;
        }
        if (recvHeadMoved) {
            mCachedHalOutputWriteHead = halWriteHead;
        }
        mCachedHalNFrames = (uint32_t)halNFrames;

        // Anchor liveness, tracked separately from the ring pair above: the
        // anchor advances on EVERY IO operation in either direction
        // (DoIOOperation publishes it for both ReadInput and WriteMix),
        // so it answers "is the HAL alive" for a whole class of sessions
        // where a head stands still — a playback-only client never runs
        // ReadInput, so the input head says nothing about the HAL's
        // health. When the anchor advances, the health delta's reference
        // point moves with it: the delta measures how far the daemon has
        // run past the last observed HAL activity, and an advancing anchor
        // resets that to now. Without this reset the delta grows without
        // bound on a perfectly healthy stack (its only writer was
        // reanchor), crosses kReanchorThresholdFrames in 85 ms, and
        // forced a spurious re-anchor every three windows.
        if (halAnchorSample != mCachedHalAnchorSample) {
            mCachedHalAnchorSample = halAnchorSample;
            mLastHalProgressFrame = FrameNumber;
        }
        // The first comparison above captures whether each head moved this
        // cycle. Do not recompute it after updating the caches.
        const RingProjector cadence = projector(nframes);
        const bool geometryValid = cadence.geometry_valid();
        if (!geometryValid && !mRingGeometryFaulted) {
            mRingGeometryFaulted = true;
            shmDriverFault->fetch_or(JB_FAULT_BAD_RING_GEOMETRY,
                                     std::memory_order_release);
            JB_LOG_ERR(jb_log_shm(),
                "invalid ring geometry: ring=%u block=%u hal=%u period=%u jitter=%d",
                cadence.ring_frames, cadence.block_frames,
                cadence.hal_block_frames, cadence.period_frames,
                cadence.jitter_frames);
        } else if (geometryValid && mRingGeometryFaulted) {
            mRingGeometryFaulted = false;
            shmDriverFault->fetch_and(~(uint64_t)JB_FAULT_BAD_RING_GEOMETRY,
                                      std::memory_order_release);
        }

        // Resync rule for the free-running upstream cursor. Runs before the
        // copy paths and the cadence counters, so this cycle reads at the
        // corrected position and the counters measure the corrected walk.
        //
        // The window comes from the two ways a read can be wrong, not from
        // the cushion:
        //   Forward limit  block + jitter - period: the position where this
        //                 cycle's read of [pos, pos+P) would touch the live
        //                 block at the write head. Between head jumps the
        //                 cursor legitimately runs up to block - period
        //                 ahead of the target — that headroom is the walk
        //                 the cursor replaced — so a tighter limit would snap
        //                 mid-walk and re-read zeroed slots every few cycles.
        //   Backward limit -block: more than one settled block of extra
        //                 latency. The data behind is still valid (only the
        //                 cursor's own consumed slots are zeroed, and they
        //                 lie behind it), so trailing costs latency, not
        //                 correctness — but past one block it is staler than
        //                 the alignment we advertise, so snap.
        // With block = max(N, P), the cursor's sawtooth against the head's
        // jump fits inside this window for every N, P and every seed
        // phase: the forward edge is the bound by construction, and the
        // backward edge holds because block >= (N+P)/2.
        //
        // Every snap is published to RECV_RESYNCS. A silent correction is
        // how a clock-rate error hides: a steady climb there means the two
        // rates differ — see docs/plan-free-running-cursor.md, section 6.
        // The dup/skip counters are not suppressed on a snap cycle: a snap
        // has a real audio cost (silence re-read forward, frames skipped
        // backward), and hiding it would let a snap pass unmeasured. Read
        // recvResyncs first: if it moved, it explains any same-window
        // movement in the other counters.
        //
        // Gated on recvHeadMoved: with the head frozen there is no live
        // write block to collide with (see the cache-update comment above),
        // so the snap rule waits for the head to move again. This is what
        // keeps a recording-only session's dupReadCycles flat instead of
        // climbing from phantom rate-error snaps.
        if (isActive && geometryValid && mRecvCursorSeeded && recvHeadMoved) {
            const int64_t recvErr = cadence.recv_error(mRecvCursor);
            if (cadence.recv_outside_window(recvErr)) {
                mRecvCursor += (uint64_t)(-recvErr);
                mRecvResyncs++;
                shmRecvResyncs->store(mRecvResyncs, std::memory_order_relaxed);
            }
        }


        // Resync rule for the free-running downstream cursor — the mirror
        // of the recv rule above, same placement and same honesty. The
        // window comes from the two ways a write can be wrong:
        //   Backward (torn) edge  err < N - block - jitter: this cycle's
        //                 write of [pos, pos+P) would land inside the
        //                 consumer's live block [H, H+N). Torn audio —
        //                 snap. N is the TRUE HAL block
        //                 (cadence.hal_block_frames), not the clearance.
        //   Forward (lap) edge    err > ring - block - jitter - P: the
        //                 write would wrap a full ring onto the consumer's
        //                 current or next block — overwrite audio it has
        //                 not consumed. Snap.
        // There is deliberately no forward discipline edge like recv's: a
        // stalled consumer is absorbed by the cursor walking ahead
        // open-loop (the delta term's one good behaviour, kept), so the
        // window is wide and a snap means a real hazard, not a hiccup.
        // A reanchor that plants the cursor just before a head jump can
        // cross the torn edge once — one bounded snap — then healthy.
        //
        // Gated on sendHeadMoved for the same reason as the recv rule: a
        // playback-only session never runs ReadInput, the head is frozen,
        // there is no live read block to tear into, and the cursor's
        // open-loop walk is correct. Snapping against the frozen head would
        // lap the ring every few hundred cycles and climb dupWriteCycles in
        // a healthy session.
        //
        // Send snaps remain os_log-only until the next protocol bump adds a
        // control slot and updates ShmReader and the runbook. Their audio cost
        // remains visible in dupWrite/skipWrite on the snap cycle.
        if (isActive && geometryValid && mSendCursorSeeded && sendHeadMoved) {
            const int64_t sendErr = cadence.send_error(mSendCursor);
            if (cadence.send_outside_window(sendErr)) {
                mSendCursor += (uint64_t)(-sendErr);
                mSendResyncs++;
            }
        }
        // Cadence is measured from the absolute cursor positions used by the
        // copy loops. A delta of P is healthy; a smaller delta is a duplicate
        // event, and a larger delta counts skipped frames.
        //
        // A forward snap contributes its displacement to skip frames. A
        // backward snap is one dup event because that counter has no frame
        // magnitude. The old masked-position gap misread it as a huge forward
        // skip of ring - k.
        //
        // Keyed on positions, not heads: both cursors walk forward on
        // their own while a head stands still (a playback-only session
        // never advances the input head at all), so "the head did not
        // move" says nothing about whether the position repeated.
        //
        // Counted here, once per JACK cycle. Pure arithmetic and at most
        // two relaxed stores -- no allocation, no syscall, no lock.
        //
        // Skips are counted in frames, not events: one cycle that stepped
        // over four blocks matters four times as much as one that stepped
        // over a single block, and an event count hides that.
        if (isActive && mCadencePrimed) {
            const int64_t period = (int64_t)(nframes > 0 ? nframes : 0);

            const int64_t recvGap =
                (int64_t)(mRecvCursor - mPrevRecvCursor);
            if (recvGap < period) {
                mDupReadCycles++;
                shmDupReadCycles->store(mDupReadCycles, std::memory_order_relaxed);
            } else if (recvGap > period) {
                mSkipReadFrames += (uint64_t)(recvGap - period);
                shmSkipReadFrames->store(mSkipReadFrames, std::memory_order_relaxed);
            }

            const int64_t sendGap =
                (int64_t)(mSendCursor - mPrevSendCursor);
            if (sendGap < period) {
                mDupWriteCycles++;
                shmDupWriteCycles->store(mDupWriteCycles, std::memory_order_relaxed);
            } else if (sendGap > period) {
                mSkipWriteFrames += (uint64_t)(sendGap - period);
                shmSkipWriteFrames->store(mSkipWriteFrames, std::memory_order_relaxed);
            }
        }
        // Prime on the first active cycle: the jump from 0 to the first
        // live position is not a skip, and counting it would put a large
        // constant in every reading.
        mPrevRecvCursor = mRecvCursor;
        mPrevSendCursor = mSendCursor;
        if (isActive) mCadencePrimed = true;

        // Liveness, tracked separately from the ring pair above. The anchor
        // advances on every IO operation in either direction.
        if (isActive) {
            // delta = how far the daemon has advanced since the HAL last ran a
            // cycle. Steady state: the anchor advances once per CoreAudio
            // cycle, and because the daemon samples it once per JACK cycle
            // the delta sawtooths 0..N even on a perfectly live HAL — the
            // threshold must sit above that sawtooth (N + margin), or every
            // window at a large N reads as stalled. During a real HAL stall
            // the delta grows past it by nframes per JACK cycle.
            //
            // Measured against the anchor, not against halInputReadHead: the
            // read head stands still for a whole class of healthy sessions
            // (any playback-only client), and treating that as a stall is what
            // produced continuous divergence re-anchors on a HAL that was
            // delivering every cycle on time.
            uint64_t delta = FrameNumber - mLastHalProgressFrame;
            if (delta > mHealthDeltaMax) mHealthDeltaMax = delta;
            const uint64_t snapThreshold =
                (uint64_t)mCachedHalNFrames + (uint64_t)kSnapThresholdFrames;
            if (delta > snapThreshold)
                mSnapCount.fetch_add(1, std::memory_order_relaxed);
        }

        uint64_t period = (uint64_t)SampleRate * 5;
        if (period && FrameNumber / period != lastTraceFrame / period) {
            uint32_t xruns = mXRunCount.exchange(0, std::memory_order_relaxed);
            uint32_t snaps = mSnapCount.exchange(0, std::memory_order_relaxed);
            // Publish the window before logging it. os_log was the only place
            // these two ever appeared, which is how a stack could sit hours
            // out of anchor and still show green in `just shm` and the menu
            // bar (docs/plan-tuning.md 2.9).
            shmHealthDeltaMax->store(mHealthDeltaMax, std::memory_order_relaxed);
            shmHealthSnaps->store((uint64_t)snaps, std::memory_order_release);
            JB_LOG_INFO(jb_log_shm(),
                "health xruns=%u deltaMax=%llu snaps=%u sendResyncs=%llu recvResyncs=%llu",
                (unsigned)xruns,
                (unsigned long long)mHealthDeltaMax,
                (unsigned)snaps,
                (unsigned long long)mSendResyncs,
                (unsigned long long)mRecvResyncs);
            // Bound the snap. Below the threshold, snapping converges and this
            // stays at 0; above it, snapping cannot converge, so stop waiting
            // for it and re-anchor instead.
            if (mHealthDeltaMax > kReanchorThresholdFrames) {
                if (++mDivergedWindows >= kReanchorWindows) {
                    JB_LOG_DEFAULT(jb_log_shm(),
                        "timeline diverged: deltaMax=%llu frames for %u windows — re-anchoring",
                        (unsigned long long)mHealthDeltaMax, kReanchorWindows);
                    reanchor("divergence", nframes);   // clears the health window
                }
            } else {
                mDivergedWindows = 0;
            }
            mHealthDeltaMax = 0;
        }
        lastTraceFrame = FrameNumber;
    }
};

int
main(int argc, char** argv)
{
    JackBridge* jackBridge[NUM_INSTANCES];
    int ch, num_midiIn=-1, num_midiOut=-1;
    bool vflag=false;

    // Block SIGINT/SIGTERM on every thread so they get delivered exclusively
    // via sigwait() below. JACK threads inherit this mask, so the on_shutdown
    // callback can raise SIGTERM and we'll catch it here for clean teardown.
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    sigaddset(&sigset, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    // Capture before any JACK threads spawn so the port-registration callback
    // can pthread_kill us awake.
    g_main_thread = pthread_self();

    g_jitter_frames = config_plist_long("JitterFrames", kDefaultJitterFrames,
                                        0, kMaxJitterFrames);
    // The pi's netadapter pair. We do not run netadapter — jackbridge-ctl
    // pushes these to the pi — but the HAL's latency model needs them, and
    // config.plist is the one place they are written down.
    g_net_latency_cycles = config_plist_long("NetLatency", JB_NET_LATENCY_CYCLES, 1, 30);
    g_net_ring_frames    = config_plist_long("NetRing", JB_NETADAPTER_RING_FRAMES, 64, 65536);
    g_workgroup_mode = config_workgroup_mode();

    // libjack reads JACK_NO_WORKGROUP when the client's realtime thread starts,
    // inside jack_client_open -> activate, so this must precede every client.
    if (g_workgroup_mode != WorkgroupMode::Backend) {
        setenv("JACK_NO_WORKGROUP", "1", 1);
    } else {
        unsetenv("JACK_NO_WORKGROUP");
    }
    JB_LOG_DEFAULT(jb_log_daemon(),
                   "config: JitterFrames=%ld Workgroup=%{public}s NetLatency=%ld NetRing=%ld",
                   g_jitter_frames, workgroup_mode_name(g_workgroup_mode),
                   g_net_latency_cycles, g_net_ring_frames);

    while ((ch = getopt(argc, argv, "vi:o:")) != -1) {
        switch (ch) {
            case 'v':
                vflag = true;
                break;
#ifdef _WITH_MIDI_BRIDGE_
            case 'i':
                num_midiIn = atoi(optarg);
                if (num_midiIn > MAX_MIDI_PORTS) {
                    fprintf(stderr, "%s: exceed maximum MIDI Inputs number (> %d)\n", argv[0], MAX_MIDI_PORTS);
                }
                break;

            case 'o':
                num_midiOut = atoi(optarg);
                if (num_midiOut > MAX_MIDI_PORTS) {
                    fprintf(stderr, "%s: exceed maximum MIDI Outputs number (> %d)\n", argv[0], MAX_MIDI_PORTS);
                }
                break;
#endif
             default:
                fprintf(stderr, "Usage: %s [-v] [-i <# of MIDI-In>] [-o <# of MIDI-Out>]\n", argv[0]);
                return -1;
        }
    }

    // Create instances of jack client
    jackBridge[0] = new JackBridge("JackBridge #1", 0, num_midiIn, num_midiOut);
    if (vflag) {
        jackBridge[0]->setVerbose(vflag);
    }

    //jackBridge[1] = new JackBridge("JackBridge #2", 1);

    // activate gateway from/to jack ports
    jackBridge[0]->activate();
    //jackBridge[1]->activate();

    // After activation, pick up any slave ports that registered before us.
    // Slaves that connect later are picked up by the port-registration callback.
    jackBridge[0]->auto_wire();
    jackBridge[0]->publish_slave_health();

    // Event loop. SIGUSR1 = slave ports changed, run auto_wire on the main
    // thread (legal context for jack_connect). SIGINT/SIGTERM = teardown.
    int sig = 0;
    while (true) {
        sigwait(&sigset, &sig);
        if (sig == SIGUSR1) {
            if (g_wire_dirty.exchange(false, std::memory_order_acq_rel)) {
                jackBridge[0]->auto_wire();
                jackBridge[0]->publish_slave_health();
            }
            continue;
        }
        break;
    }
    JB_LOG_DEFAULT(jb_log_daemon(), "caught signal %d, shutting down", sig);

    delete jackBridge[0];
    // Don't shm_unlink — the HAL is the shm owner; unlinking would force a
    // recreate cycle on its side. The HAL's staleness watchdog handles our
    // departure via the zeroed heartbeat.
    return 0;
}
