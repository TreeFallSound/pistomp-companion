// jbdump — print the JackBridge shm control fields. Read-only.
//
// The daemon and the HAL driver share one POSIX region, /JackBridge, and every
// piece of state that decides whether audio flows lives in it at a fixed
// offset. Nothing else shows you the current values: the menu bar renders an
// interpretation of them as one coloured dot, and os_log reports transitions
// but never the value now. When the link is up, the ports are wired, and there
// is still silence, this is the tool that says which side is wrong.
//
// It maps the region PROT_READ from an O_RDONLY descriptor, so it cannot
// perturb the audio path, and it can run while everything is live.
//
// Offsets come from JackBridge.h. Do not hand-copy them here: the point of the
// single source of truth is that a protocol bump cannot silently desync this
// tool from the region it reads.
//
// Build: see the `shm` recipe in the justfile.

#include <cstdio>
#include <cstring>
#include <cinttypes>
#include "JackBridge.h"

namespace {

// The region is written by two processes on live audio threads. Read each
// field once, acquire, exactly as the daemon and driver do -- a torn or
// re-ordered read here would invent a state that never existed.
uint64_t field(const char* base, size_t off)
{
    return reinterpret_cast<const std::atomic<uint64_t>*>(base + off)
               ->load(std::memory_order_acquire);
}

const char* driver_status_name(uint64_t v)
{
    switch (v) {
        case JB_DRV_STATUS_INIT:    return "INIT";
        case JB_DRV_STATUS_ACTIVE:  return "ACTIVE";
        case JB_DRV_STATUS_STARTED: return "STARTED";
        default:                    return "unknown";
    }
}

void print_fault(uint64_t v)
{
    if (v == 0) { printf("  driverFault           0 (none)\n"); return; }
    printf("  driverFault           0x%" PRIx64, v);
    if (v & JB_FAULT_DEVICE_NOT_ALIVE) printf(" DEVICE_NOT_ALIVE(feeding silence)");
    printf("\n");
}

} // namespace

int main(int argc, char** argv)
{
    unsigned instance = 0;
    if (argc > 1) instance = (unsigned)strtoul(argv[1], NULL, 10);
    if (instance >= NUM_INSTANCES) {
        fprintf(stderr, "jbdump: instance must be 0..%u\n", NUM_INSTANCES - 1);
        return 2;
    }

    int fd = shm_open(JACK_SHMPATH, O_RDONLY);
    if (fd < 0) {
        // The driver creates the region in _HW_Open, which runs only when
        // coreaudiod loads the plug-in. ENOENT therefore means the plug-in is
        // not loaded -- not that the daemon is down.
        fprintf(stderr, "jbdump: shm_open(%s) failed: %s\n",
                JACK_SHMPATH, strerror(errno));
        if (errno == ENOENT)
            fprintf(stderr, "jbdump: the HAL plug-in is not loaded. "
                            "Try `just restart`, then check `pgrep -fl \"Core Audio Driver\"`.\n");
        return 1;
    }

    const char* base = (const char*)mmap(NULL, REGSMAP_SIZE, PROT_READ,
                                         MAP_SHARED, fd, instance * REGSMAP_BOUNDARY);
    if (base == MAP_FAILED) {
        fprintf(stderr, "jbdump: mmap failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    uint64_t proto = field(base, JB_OFF_PROTOCOL_VERSION);
    printf("instance %u  (%s)\n", instance, JACK_SHMPATH);
    printf("  protocolVersion       %" PRIu64 "%s\n", proto,
           proto == JACKBRIDGE_PROTOCOL_VERSION ? ""
                                                : "  *** MISMATCH with this build ***");

    uint64_t status = field(base, JB_OFF_DRIVER_STATUS);
    printf("  driverStatus          %" PRIu64 " (%s)\n", status, driver_status_name(status));
    print_fault(field(base, JB_OFF_DRIVER_FAULT));

    // syncMode decides which process owns the timeline, and reading the wrong
    // one costs you a whole diagnosis: in sync mode (1, the only mode shipped)
    // the daemon publishes the anchor and GetZeroTimeStamp only relays it, so
    // the driver's gDevice_* globals have no effect at all.
    uint64_t sync = field(base, JB_OFF_SYNC_MODE);
    printf("  syncMode              %" PRIu64 " (%s owns the timeline)\n",
           sync, sync == 1 ? "daemon" : "driver free-runs");

    printf("  daemonAlive           %" PRIu64 "   (heartbeat; must advance)\n",
           field(base, JB_OFF_DAEMON_ALIVE));
    printf("  slavePortsConnected   %" PRIu64 " of 6\n",
           field(base, JB_OFF_SLAVE_PORTS_CONNECTED));
    printf("  daemonXRuns           %" PRIu64 "\n", field(base, JB_OFF_DAEMON_XRUNS));
    printf("  resyncRequest         %" PRIu64 "   (app -> daemon+driver; both re-anchor on a new nonce)\n",
           field(base, JB_OFF_RESYNC_REQUEST));
    printf("  reanchorCount         %" PRIu64 "   (re-anchors since the daemon started)\n",
           field(base, JB_OFF_REANCHOR_COUNT));

    // The timeline health window. These two were os_log-only until protocol
    // 10, which is how a stack could be hours out of anchor and still show
    // STARTED, no fault, a live heartbeat and 6 of 6 ports. deltaMax near 0
    // and snaps at 0 is the only shape that means the timeline is holding --
    // check them before and after any measurement window.
    uint64_t deltaMax = field(base, JB_OFF_HEALTH_DELTA_MAX);
    uint64_t snaps    = field(base, JB_OFF_HEALTH_SNAPS);
    printf("  healthDeltaMax        %" PRIu64 " frames%s\n", deltaMax,
           deltaMax ? "   *** timeline is not holding ***" : "");
    printf("  healthSnaps           %" PRIu64 "%s\n", snaps,
           snaps ? "   *** timeline is not holding ***" : "");

    printf("  halNFrames            %" PRIu64 "\n", field(base, JB_OFF_HAL_NFRAMES));
    printf("  halSampleRate         %" PRIu64 "\n", field(base, JB_OFF_HAL_SAMPLE_RATE));
    uint64_t period = field(base, JB_OFF_JACK_PERIOD_FRAMES);
    uint64_t rate   = field(base, JB_OFF_JACK_SAMPLE_RATE);
    printf("  jackPeriodFrames      %" PRIu64 "\n", period);
    printf("  jackSampleRate        %" PRIu64 "\n", rate);

    // The advertised-latency inputs, and the figure they produce. The daemon
    // and the driver each log this number; if the three disagree, one of them
    // is running against a stale binary or a stale pair.
    uint64_t netL = field(base, JB_OFF_NET_LATENCY_CYCLES);
    uint64_t netG = field(base, JB_OFF_NET_RING_FRAMES);
    printf("  jitterFrames          %" PRIu64 "   (SafetyOffset)\n",
           field(base, JB_OFF_JITTER_FRAMES));
    printf("  netLatencyCycles      %" PRIu64 "   (netadapter -l)%s\n", netL,
           netL ? "" : "   (daemon has not published yet)");
    printf("  netRingFrames         %" PRIu64 "   (netadapter -g)%s\n", netG,
           netG ? "" : "   (daemon has not published yet)");
    printf("  oneWayLatency         %u frames   (derived; monitoring trip %u)\n",
           jb_one_way_latency_frames(period, rate, netL, netG),
           2 * jb_one_way_latency_frames(period, rate, netL, netG));

    // The write head advances on a playback or monitoring session. The read
    // head is mInputTime.mSampleTime, which CoreAudio leaves at 0 unless the
    // host actually pulls input -- a 0 here is not by itself a fault.
    printf("  halInputReadHead      %" PRIu64 "\n", field(base, JB_OFF_HAL_INPUT_READ_HEAD));
    printf("  halOutputWriteHead    %" PRIu64 "\n", field(base, JB_OFF_HAL_OUTPUT_WRITE_HEAD));
    printf("  numberTimeStamps      %" PRIu64 "\n", field(base, JB_OFF_NUMBER_TIMESTAMPS));
    printf("  zeroHostTime          0x%" PRIx64 "\n", field(base, JB_OFF_ZERO_HOST_TIME));
    printf("  seed                  %" PRIu64 "\n", field(base, JB_OFF_SEED));

    char name[JB_DEVICE_NAME_MAX + 1];
    memcpy(name, base + JB_OFF_DEVICE_NAME, JB_DEVICE_NAME_MAX);
    name[JB_DEVICE_NAME_MAX] = '\0';
    printf("  deviceName            '%s'%s\n", name,
           name[0] ? "" : "   (daemon has not published yet)");

    munmap((void*)base, REGSMAP_SIZE);
    close(fd);
    return 0;
}
