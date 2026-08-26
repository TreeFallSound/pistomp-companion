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

// Set in main() before jack_activate; read by the port-registration callback to
// wake the main thread out of sigwait when slave ports come or go. Notification
// callbacks are forbidden from calling jack_connect (JACK aborts with
// "Cannot callback the server in notification thread"), so we defer the wiring
// pass to the main thread via SIGUSR1.
static pthread_t g_main_thread;
static std::atomic<bool> g_wire_dirty{false};

// Daemon-side safety margin in frames, read once at startup from config.plist.
// The HAL realises this by returning JitterFrames from
// kAudioDevicePropertySafetyOffset; CoreAudio then schedules the IOProc that
// many frames earlier in sampleTime, so the daemon's write head naturally
// sits JitterFrames ahead of the HAL's read head in the ring.
static constexpr long kDefaultJitterFrames = 192;
static long g_jitter_frames = kDefaultJitterFrames;

// Drift threshold (frames) at which |FrameNumber - (halReadHead + JitterFrames)|
// exceeds the safety window. Counted in the 5s drift trace as a diagnostic for
// scheduler hiccups large enough to consume the SafetyOffset lead.
static constexpr int64_t kSnapThresholdFrames = 512;

// Reads a long-valued key from /Library/Application Support/JackBridge/config.plist
// via PlistBuddy. Returns `def` if the file/key is missing or unparseable.
// Runs once at startup; popen cost is irrelevant outside the realtime path.
static long read_config_long(const char* key, long def) {
    char cmd[512];
    snprintf(cmd, sizeof cmd,
        "/usr/libexec/PlistBuddy -c 'Print :%s' "
        "'/Library/Application Support/JackBridge/config.plist' 2>/dev/null",
        key);
    FILE* f = popen(cmd, "r");
    if (!f) return def;
    long val = def;
    if (fscanf(f, "%ld", &val) != 1) val = def;
    pclose(f);
    return val;
}
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
        FramesPerBuffer = STRBUFNUM/2;
        shmBufferSize->store(STRBUFSZ, std::memory_order_release);
        shmSyncMode->store(0, std::memory_order_release);

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

        lastTraceFrame = 0;

        // Best-effort workgroup acquisition. May fail if Core Audio hasn't
        // surfaced our HAL device yet (e.g. coreaudiod is mid-rescan) — the
        // process callback retries until it succeeds. Joining the workgroup
        // happens lazily on the JACK RT thread because os_workgroup_join must
        // run on the joining thread itself.
        mWorkgroup = workgroup_acquire_by_uid(JACKBRIDGE_DEVICE_UID);
        mWorkgroupJoined = false;
        mWorkgroupAcquireBackoff = 0;

        JB_LOG_INFO(jb_log_daemon(),
            "JackBridge#%u: start sr=%d Hz, bufsize=%u bytes, jitter=%ld frames",
            instance, SampleRate, (unsigned)BufSize, g_jitter_frames);
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
                    "/Library/Application Support/JackBridge/config.plist to a different "
                    "device (e.g. built-in output). See docs/idiosyncrasies.md.",
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
        // flips DeviceIsAlive=0 so the DAW disconnects instead of getting
        // forever-silence. relaxed is fine: the staleness check only cares
        // that the value moves, not what the value is.
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
            // Re-anchor: map current wall-clock time to the current frozen
            // FrameNumber so the HAL resumes reading from where we're writing.
            // Do NOT reset FrameNumber — that would create a timeline
            // discontinuity and break transport progression.
            shmZeroHostTime->store(mach_absolute_time(),
                                   std::memory_order_relaxed);
            shmNumberTimeStamps->store(FrameNumber / FramesPerBuffer,
                                           std::memory_order_release);
            shmSeed->fetch_add(1, std::memory_order_release);
            JB_LOG_INFO(jb_log_jack(),
                "JackBridge#%u: re-anchored after HAL restart frame=%llu",
                instance, (unsigned long long)FrameNumber);
        }
        mLastDriverStatus = currentStatus;

        if (currentStatus != JB_DRV_STATUS_STARTED) {
            // Driver isn't working. Just return zero buffer;
            for(int i=0; i<NUM_OUTPUT_CHANNELS; i++) {
                aout[i] = (sample_t*)jack_port_get_buffer(audioOut[i], nframes);
                bzero(aout[i], STRBUFSZ);
            }
            return 0;
        }

        // For DEBUG
        check_progress();

        if (!isActive) {
            ncalls = 0;
            FrameNumber = 0;

            if (isSyncMode) {
                shmSyncMode->store(1, std::memory_order_relaxed);
                shmNumberTimeStamps->store(0, std::memory_order_relaxed);
                shmSeed->fetch_add(1, std::memory_order_release);
            }

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

        if ((FrameNumber % FramesPerBuffer) == 0) {
            shmZeroHostTime->store(mach_absolute_time(),
                                   std::memory_order_relaxed);
            shmNumberTimeStamps->store(FrameNumber / FramesPerBuffer,
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
    // wired to JackBridge input_<n>, etc. Multi-slave fan-out and an opt-out
    // are deferred to config.plist (PLAN.md §3.4.3).
    void auto_wire() {
        wire_direction("from_slave", JackPortIsOutput, audioIn, nAudioIn);
        wire_direction("to_slave",   JackPortIsInput,  audioOut, nAudioOut);
    }

    void on_shutdown() override {
        // Called by jackd when it goes away (intentional stop, crash, whatever).
        // Zero the heartbeat so the HAL's staleness watchdog flips DeviceIsAlive
        // immediately rather than waiting for the 5-cycle threshold, then nudge
        // main()'s sigwait so we exit cleanly. LaunchAgent KeepAlive brings us
        // back when jackd is back.
        shmDaemonAlive->store(0, std::memory_order_release);
        shmDriverStatus->store(JB_DRV_STATUS_INIT, std::memory_order_release);
        JB_LOG_DEFAULT(jb_log_jack(), "jackd shut down — exiting for LaunchAgent restart");
        kill(getpid(), SIGTERM);
    }

    static void _port_registration_callback(jack_port_id_t port_id, int registered, void* arg) {
        // Only react to port appearances, not departures — we don't need to
        // disconnect anything when a slave goes away (jackd handles that).
        if (!registered) return;
        JackBridge* self = (JackBridge*)arg;
        jack_port_t* port = jack_port_by_id(self->client, port_id);
        if (!port) return;
        const char* shortname = jack_port_short_name(port);
        if (!shortname) return;
        if (strncmp(shortname, "from_slave_", 11) == 0 ||
            strncmp(shortname, "to_slave_", 9)   == 0) {
            // Defer to main thread — jack_connect is illegal from here.
            g_wire_dirty.store(true, std::memory_order_release);
            pthread_kill(g_main_thread, SIGUSR1);
        }
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

    // RT-safe event counters drained by check_progress() every 5s. xruns come
    // from jackd's xrun_callback; snaps are cycles where the FrameNumber-vs-
    // halReadHead margin drifted >kSnapThresholdFrames from JitterFrames.
    std::atomic<uint32_t> mXRunCount{0};

    // Health-window accumulators, single-writer (JACK process thread via
    // check_progress). All zero in healthy steady state.
    uint64_t mHealthDeficit{0};        // Σ max(0, JitterFrames - margin) per cycle
    uint64_t mHealthMarginAbsDev{0};   // Σ |margin - JitterFrames| per cycle
    std::atomic<uint32_t> mSnapCount{0};

    int sendToCoreAudio(float** in, int nframes) {
        // FIXME: should be consider buffer overwrapping
        // Offset arithmetic lives in RingProjector (testable in isolation).
        // Today this is open-loop: frame_cursor % ring_frames, with no
        // reference to the HAL's read head. See investigation-bug1.md and
        // the TODO in RingProjector::send_offset.
        const RingProjector proj{ (uint32_t)FramesPerBuffer, FrameNumber };
        const uint32_t offset = proj.send_offset();
        for (int j = 0; j < NUM_INPUT_STREAMS; j++) {
            ring_write_stereo_interleaved(
                buf_down[j], (uint32_t)FramesPerBuffer, offset,
                in[j*2 + 0], in[j*2 + 1], nframes);
        }
        return nframes;
    }

    int receiveFromCoreAudio(float** out, int nframes) {
        // FIXME: should be consider buffer overwrapping
        // Offset arithmetic lives in RingProjector (testable in isolation).
        // The (frame_cursor - nframes) expression is unsigned-arithmetic
        // underflow-by-design on the first cycle after activation — preserved
        // bug-for-bug from the inline code. See RingProjector::recv_offset.
        const RingProjector proj{ (uint32_t)FramesPerBuffer, FrameNumber };
        const uint32_t offset = proj.recv_offset(nframes);
        for (int j = 0; j < NUM_OUTPUT_STREAMS; j++) {
            ring_consume_stereo_interleaved(
                buf_up[j], (uint32_t)FramesPerBuffer, offset,
                out[j*2 + 0], out[j*2 + 1], nframes);
        }
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

    // Health trace. Per-cycle: snapshot the HAL's read head, compute the
    // margin (FrameNumber - halReadHead — should sit at JitterFrames), and
    // accumulate integrated deviation and deficit. Emit one line every ~5s.
    //
    // All four values are zero in healthy steady state. Any nonzero value is
    // the only thing worth reading.
    //
    // Tail with:
    //   log stream --predicate 'subsystem == "com.jackbridge" && category == "shm"'
    void check_progress() {
        uint64_t halReadHead = 0;
        uint64_t s1, s2;
        do {
            s1 = shmHalAnchorSeq->load(std::memory_order_acquire);
            halReadHead = shmHalInputReadHead->load(std::memory_order_relaxed);
            s2 = shmHalAnchorSeq->load(std::memory_order_acquire);
        } while ((s1 & 1) || s1 != s2);

        if (halReadHead > 0 && isActive) {
            int64_t margin = (int64_t)FrameNumber - (int64_t)halReadHead;
            int64_t delta  = margin - (int64_t)g_jitter_frames;
            uint64_t adelta = (uint64_t)(delta < 0 ? -delta : delta);
            mHealthMarginAbsDev += adelta;
            if (delta < 0) mHealthDeficit += (uint64_t)(-delta);
            if (adelta > (uint64_t)kSnapThresholdFrames) {
                mSnapCount.fetch_add(1, std::memory_order_relaxed);
            }
        }

        uint64_t period = (uint64_t)SampleRate * 5;
        if (period && FrameNumber / period != lastTraceFrame / period) {
            uint32_t xruns = mXRunCount.exchange(0, std::memory_order_relaxed);
            uint32_t snaps = mSnapCount.exchange(0, std::memory_order_relaxed);
            JB_LOG_INFO(jb_log_shm(),
                "health xruns=%u deficit=%llu marginAbsDev=%llu snaps=%u",
                (unsigned)xruns,
                (unsigned long long)mHealthDeficit,
                (unsigned long long)mHealthMarginAbsDev,
                (unsigned)snaps);
            mHealthDeficit = 0;
            mHealthMarginAbsDev = 0;
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

    // Read tunables from config.plist before any RT code runs.
    g_jitter_frames = read_config_long("JitterFrames", kDefaultJitterFrames);
    if (g_jitter_frames < 0) g_jitter_frames = kDefaultJitterFrames;
    JB_LOG_DEFAULT(jb_log_daemon(), "config: JitterFrames=%ld", g_jitter_frames);

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

    // Event loop. SIGUSR1 = slave ports changed, run auto_wire on the main
    // thread (legal context for jack_connect). SIGINT/SIGTERM = teardown.
    int sig = 0;
    while (true) {
        sigwait(&sigset, &sig);
        if (sig == SIGUSR1) {
            if (g_wire_dirty.exchange(false, std::memory_order_acq_rel)) {
                jackBridge[0]->auto_wire();
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
