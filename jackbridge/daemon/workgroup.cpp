#include "workgroup.hpp"
#include "jb_log.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <errno.h>
#include <vector>

static AudioObjectID find_device_by_uid(const char* device_uid) {
    AudioObjectPropertyAddress devicesAddr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    OSStatus st = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
        &devicesAddr, 0, NULL, &dataSize);
    if (st != noErr || dataSize == 0) return kAudioObjectUnknown;

    UInt32 count = dataSize / sizeof(AudioObjectID);
    std::vector<AudioObjectID> ids(count);
    st = AudioObjectGetPropertyData(kAudioObjectSystemObject,
        &devicesAddr, 0, NULL, &dataSize, ids.data());
    if (st != noErr) return kAudioObjectUnknown;

    AudioObjectPropertyAddress uidAddr = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    CFStringRef want = CFStringCreateWithCString(NULL, device_uid, kCFStringEncodingUTF8);
    AudioObjectID result = kAudioObjectUnknown;
    for (UInt32 i = 0; i < count; ++i) {
        CFStringRef uid = NULL;
        UInt32 sz = sizeof(uid);
        if (AudioObjectGetPropertyData(ids[i], &uidAddr, 0, NULL, &sz, &uid) == noErr && uid) {
            if (CFStringCompare(uid, want, 0) == kCFCompareEqualTo) {
                result = ids[i];
                CFRelease(uid);
                break;
            }
            CFRelease(uid);
        }
    }
    CFRelease(want);
    return result;
}

os_workgroup_t workgroup_acquire_by_uid(const char* device_uid) {
    AudioObjectID dev = find_device_by_uid(device_uid);
    if (dev == kAudioObjectUnknown) {
        JB_LOG_INFO(jb_log_daemon(),
            "workgroup: device with UID '%{public}s' not found yet", device_uid);
        return NULL;
    }

    AudioObjectPropertyAddress wgAddr = {
        kAudioDevicePropertyIOThreadOSWorkgroup,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    os_workgroup_t wg = NULL;
    UInt32 sz = sizeof(wg);
    OSStatus st = AudioObjectGetPropertyData(dev, &wgAddr, 0, NULL, &sz, &wg);
    if (st != noErr || !wg) {
        JB_LOG_ERR(jb_log_daemon(),
            "workgroup: device %u present but IOThreadOSWorkgroup query failed (OSStatus=%d)",
            (unsigned)dev, (int)st);
        return NULL;
    }
    JB_LOG_DEFAULT(jb_log_daemon(),
        "workgroup: acquired for device %u (uid '%{public}s')",
        (unsigned)dev, device_uid);
    return wg;
}

// Force the calling thread into Mach's time-constraint policy. os_workgroup_join
// returns EINVAL on threads that are not "realtime" by Mach's definition; on
// macOS the only way in is THREAD_TIME_CONSTRAINT_POLICY. JACK2 already does
// this for its process thread, but doing it again with the right period is
// idempotent and protects us if libjack ever changes.
static void set_time_constraint_policy(uint64_t period_ns, uint64_t computation_ns) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);

    auto ns_to_abs = [&](uint64_t ns) -> uint32_t {
        // abs = ns * denom / numer. Clamp to UINT32_MAX defensively.
        long double v = (long double)ns * (long double)tb.denom / (long double)tb.numer;
        if (v > (long double)UINT32_MAX) return UINT32_MAX;
        return (uint32_t)v;
    };

    thread_time_constraint_policy_data_t pol;
    pol.period      = ns_to_abs(period_ns);
    pol.computation = ns_to_abs(computation_ns);
    pol.constraint  = ns_to_abs(period_ns);
    pol.preemptible = 0;

    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()),
        THREAD_TIME_CONSTRAINT_POLICY,
        (thread_policy_t)&pol,
        THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) {
        JB_LOG_ERR(jb_log_daemon(),
            "workgroup: thread_policy_set THREAD_TIME_CONSTRAINT_POLICY failed kr=%d",
            (int)kr);
    } else {
        JB_LOG_DEFAULT(jb_log_daemon(),
            "workgroup: THREAD_TIME_CONSTRAINT_POLICY set "
            "period=%u computation=%u constraint=%u abs ticks (=%llu/%llu/%llu ns)",
            pol.period, pol.computation, pol.constraint,
            (unsigned long long)period_ns,
            (unsigned long long)computation_ns,
            (unsigned long long)period_ns);
    }

    // Read back the QoS class we currently advertise on this thread. This is
    // independent of THREAD_TIME_CONSTRAINT_POLICY (Mach scheduler) — QoS is
    // Apple's userland tier and influences App Nap / E-core scheduling. We
    // only LOG it; setting it is a separate decision we haven't taken.
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    int relpri = 0;
    if (pthread_get_qos_class_np(pthread_self(), &qos, &relpri) == 0) {
        const char* qos_name = "?";
        switch (qos) {
            case QOS_CLASS_USER_INTERACTIVE: qos_name = "USER_INTERACTIVE"; break;
            case QOS_CLASS_USER_INITIATED:   qos_name = "USER_INITIATED";   break;
            case QOS_CLASS_DEFAULT:          qos_name = "DEFAULT";          break;
            case QOS_CLASS_UTILITY:          qos_name = "UTILITY";          break;
            case QOS_CLASS_BACKGROUND:       qos_name = "BACKGROUND";       break;
            case QOS_CLASS_UNSPECIFIED:      qos_name = "UNSPECIFIED";      break;
        }
        JB_LOG_DEFAULT(jb_log_daemon(),
            "workgroup: QoS class on RT thread = %{public}s (relpri=%d)",
            qos_name, relpri);
    }
}

int workgroup_join_self(os_workgroup_t wg,
                        os_workgroup_join_token_s* token,
                        uint64_t period_ns,
                        uint64_t computation_ns) {
    set_time_constraint_policy(period_ns, computation_ns);

    int rc = os_workgroup_join(wg, token);
    if (rc != 0) {
        JB_LOG_ERR(jb_log_daemon(),
            "workgroup: os_workgroup_join failed rc=%d (%s)",
            rc, strerror(rc));
        return rc;
    }
    JB_LOG_DEFAULT(jb_log_daemon(),
        "workgroup: joined (period_ns=%llu computation_ns=%llu)",
        (unsigned long long)period_ns, (unsigned long long)computation_ns);
    return 0;
}

void workgroup_leave_self(os_workgroup_t wg, os_workgroup_join_token_s* token) {
    if (!wg) return;
    os_workgroup_leave(wg, token);
}
