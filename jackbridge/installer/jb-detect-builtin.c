// jb-detect-builtin — print the built-in output device's CoreAudio UID to
// stdout, or exit non-zero with a stderr explanation.
//
// Used by installer/jackd-launch when config.plist's ClockDeviceUID is empty.
// See PLAN.md §3.1 and docs/architecture.md for why jackd's CoreAudio backend
// must be pinned to a single, stable clock device.
//
// Filter: kAudioDevicePropertyTransportType == kAudioDeviceTransportTypeBuiltIn
// AND has at least one output stream. Excludes Bluetooth, USB, HDMI, virtual
// devices (including, importantly, JackBridge itself).
//
// Exits:
//   0  — UID printed to stdout (no trailing newline-only output; just the UID)
//   1  — no built-in output device found; stderr explains
//   2  — CoreAudio API failure; stderr has the OSStatus

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>

static int has_output_streams(AudioObjectID dev) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreams,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size) != noErr) return 0;
    return size > 0;
}

static UInt32 transport_type(AudioObjectID dev) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyTransportType,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    UInt32 t = 0;
    UInt32 size = sizeof(t);
    if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &t) != noErr) return 0;
    return t;
}

static CFStringRef copy_device_uid(AudioObjectID dev) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    CFStringRef uid = NULL;
    UInt32 size = sizeof(uid);
    if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &uid) != noErr) return NULL;
    return uid;
}

int main(void) {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };

    UInt32 size = 0;
    OSStatus st = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size);
    if (st != noErr) {
        fprintf(stderr, "jb-detect-builtin: AudioObjectGetPropertyDataSize failed (%d)\n", (int)st);
        return 2;
    }

    UInt32 count = size / (UInt32)sizeof(AudioObjectID);
    AudioObjectID *devs = (AudioObjectID *)calloc(count, sizeof(AudioObjectID));
    if (!devs) {
        fprintf(stderr, "jb-detect-builtin: out of memory\n");
        return 2;
    }

    st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, devs);
    if (st != noErr) {
        fprintf(stderr, "jb-detect-builtin: AudioObjectGetPropertyData failed (%d)\n", (int)st);
        free(devs);
        return 2;
    }

    for (UInt32 i = 0; i < count; i++) {
        if (transport_type(devs[i]) != kAudioDeviceTransportTypeBuiltIn) continue;
        if (!has_output_streams(devs[i])) continue;

        CFStringRef uid = copy_device_uid(devs[i]);
        if (!uid) continue;

        char buf[256];
        if (CFStringGetCString(uid, buf, sizeof(buf), kCFStringEncodingUTF8)) {
            fputs(buf, stdout);
            CFRelease(uid);
            free(devs);
            return 0;
        }
        CFRelease(uid);
    }

    free(devs);
    fprintf(stderr,
        "jb-detect-builtin: no built-in audio output device found.\n"
        "Set ClockDeviceUID explicitly in /Library/Application Support/JackBridge/config.plist.\n"
        "See docs/macos-setup.md for how to enumerate CoreAudio UIDs.\n");
    return 1;
}
