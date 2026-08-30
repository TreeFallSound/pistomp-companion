# Plan: Mac-authoritative network latency knobs

Written 2026-08-30.

## 1. The problem

Two knobs control most of the end-to-end latency: L (netadapter `-l`,
network latency cycles) and G (netadapter `-g`, slip-ring frames). These
values live only on the pi in `/etc/default/jackbridge`. The Mac does not
know them.

This causes three problems.

**The latency model is wrong.** The function `jb_one_way_latency_frames()`
uses compile-time constants `JB_NET_LATENCY_CYCLES` and
`JB_NETADAPTER_RING_FRAMES`. Those constants say L=2 and G=1024. The pi
runs L=4 and G=1024. The HAL (Hardware Abstraction Layer) tells the DAW
(Digital Audio Workstation) that the one-way latency is 978 frames, but
the correct value is 1106 frames. The DAW aligns recorded audio with a
128-frame (2.67 ms) error per leg.

**The settings are split across two machines.** The values `JitterFrames`
and `Workgroup` are in `config.plist` and appear in the Settings window.
L and G are in `/etc/default/jackbridge` on the pi and are not mirrored on
the Mac.

**A new pi image resets L and G.** `/etc/default/jackbridge` is not in
any repository. After a reflash, the pi returns to the script defaults
(L=2, G=512). The test on 2026-08-30 showed that G=512 is unstable under
mod-host load.

## 2. Design

The Mac becomes the single source of truth for L and G, in the same way
that it is already the source of truth for `JitterFrames`.

```
config.plist (Mac)
      │
      ├─ jackbridge-ctl pi-start / pi_service start
      │     └─ SSH: write /etc/default/jackbridge on pi
      │            before: sudo systemctl start pi-stomp-jackbridge
      │
      └─ JackBridged (daemon)
            ├─ reads NetLatency and NetRing from config.plist at startup
            └─ writes shmNetLatencyCycles and shmNetRingFrames to shared memory
                     └─ HAL driver
                           └─ reads L and G from shared memory
                              calls jb_one_way_latency_frames(period, rate, L, G)
                              reports result as kAudioDevicePropertyLatency
```

The pi does not need to detect a value change. The tool `jackbridge-ctl`
overwrites `/etc/default/jackbridge` every time it starts the pi service,
so the file on the pi is always current.

## 3. Changes

### 3.1 `jackbridge/installer/config.plist`

Add two keys with the current known-good values:

```xml
<!-- netadapter -l: network latency cycles, range 1 to 30.
     Budget = L × period. Must satisfy: G / 2 > L × period.
     L=4 is the stable value under mod-host load. -->
<key>NetLatency</key>
<integer>4</integer>

<!-- netadapter -g: slip-ring frames, power of two, minimum 256.
     G / 2 is the steady-state fill.
     G=512 is unstable under mod-host load (tested 2026-08-30).
     G=1024 is the stable minimum with the current pi image. -->
<key>NetRing</key>
<integer>1024</integer>
```

Also set `JitterFrames` to 320 in the installer template, to match the
value that the home plist already holds.

### 3.2 `jackbridge/tools/jackbridge-ctl`

Add a function `push_pi_config` that reads L and G from `config.plist`
and writes `/etc/default/jackbridge` on the pi over SSH (Secure Shell).

```sh
push_pi_config() {
    net_latency=$(/usr/libexec/PlistBuddy -c "Print :NetLatency" "$CONFIG" 2>/dev/null || echo 4)
    net_ring=$(/usr/libexec/PlistBuddy    -c "Print :NetRing"    "$CONFIG" 2>/dev/null || echo 1024)
    case "$net_latency" in ''|*[!0-9]*) net_latency=4    ;; esac
    case "$net_ring"    in ''|*[!0-9]*) net_ring=1024    ;; esac
    pi_ssh "printf 'JACKBRIDGE_NET_LATENCY=%s\nJACKBRIDGE_NET_RING=%s\n' \
        '$net_latency' '$net_ring' | sudo tee /etc/default/jackbridge" >/dev/null
}
```

Call `push_pi_config` before the `systemctl start` call in both
`pi_service start` and `cmd_pi_start`. Treat a failure as a warning, not
an error, so that an unreachable pi does not block a Mac-side restart.

### 3.3 `jackbridge/shared/JackBridge.h` — shared memory fields

Add two new fields after `JB_OFF_JITTER_FRAMES (0x1d0)`:

```c
#define JB_OFF_NET_LATENCY_CYCLES  (0x1d8)
#define JB_OFF_NET_RING_FRAMES     (0x1e0)
```

Both fields fit in the existing gap between `0x1d8` and `0x200` (where
the device name starts). Add `static_assert` guards to keep them in place.

**Bump the protocol version from 9 to 10.** The daemon writes these fields
and the HAL reads them. A version mismatch would cause the HAL to read the
wrong offset and report a wrong latency.

### 3.4 `jackbridge/shared/JackBridge.h` — `jb_one_way_latency_frames()`

Add L and G as parameters and remove the compile-time constant references.
Keep the two-argument overload as a fallback for callers that do not have
shared memory (for example, `jbdump`).

```c
// Full form: called by the daemon and the HAL.
static inline uint32_t jb_one_way_latency_frames(
        uint64_t period_frames, uint64_t sample_rate,
        uint64_t net_latency_cycles, uint64_t net_ring_frames) { … }

// Fallback: uses compile-time constants. Called when shm is not available.
static inline uint32_t jb_one_way_latency_frames(
        uint64_t period_frames, uint64_t sample_rate) {
    return jb_one_way_latency_frames(period_frames, sample_rate,
                                     JB_NET_LATENCY_CYCLES,
                                     JB_NETADAPTER_RING_FRAMES);
}
```

Do not remove the constants `JB_NET_LATENCY_CYCLES` and
`JB_NETADAPTER_RING_FRAMES` until all callers use the full-form overload.

### 3.5 `jackbridge/shared/JackBridge.h` — `JackBridgeDriverIF`

Add two atomic shared-memory pointers and wire them in `attach_shm()`:

```cpp
std::atomic<uint64_t> *shmNetLatencyCycles;
std::atomic<uint64_t> *shmNetRingFrames;
```

### 3.6 `jackbridge/daemon/JackBridge.cpp`

In `main()`, read L and G from `config.plist` alongside `JitterFrames`:

```cpp
long g_net_latency = config_plist_long("NetLatency", JB_NET_LATENCY_CYCLES, 1, 30);
long g_net_ring    = config_plist_long("NetRing",    JB_NETADAPTER_RING_FRAMES, 256, 65536);
```

In the `JackBridge` constructor, write both values to shared memory. Use
the same pattern as `shmJitterFrames`. Update the startup log to include
them. Update the latency-model log to use the four-argument overload.

### 3.7 `jackbridge/driver/JackBridge/Plug-In/SA_Device.cpp`

In `_UpdateAdvertisedLatency()`, read L and G from shared memory and call
the four-argument overload:

```cpp
uint64_t L = shmNetLatencyCycles
    ? shmNetLatencyCycles->load(std::memory_order_acquire) : JB_NET_LATENCY_CYCLES;
uint64_t G = shmNetRingFrames
    ? shmNetRingFrames->load(std::memory_order_acquire)    : JB_NETADAPTER_RING_FRAMES;
jb_one_way_latency_frames(period, rate, L, G);
```

If L or G is zero (the daemon has not yet written the field), fall back to
the compile-time constants.

### 3.8 Settings UI — `app/PiStompCompanion/SettingsWindowController.swift`

Add a **Network latency** section between "Realtime scheduling" and
"Audio status":

| Label | Control | Values |
|-------|---------|--------|
| Network latency (L) | popup | 1, 2, 3, 4, 5, 6, 8 cycles |
| Slip ring (G) | popup | 512, 1024, 2048, 4096 frames |

Add a note: "G=512 is unstable under mod-host load. Do not reduce L
below 4 until `daemonXruns=0` is stable in all health windows."

In `ConfigStore`, add `"NetLatency": 4` and `"NetRing": 1024` to
`defaults`, and add both keys to the `populate()` and `apply()` round-trip,
following the same pattern as `JitterFrames`.

When the user clicks Apply, the Settings window writes `config.plist` and
restarts the daemon (existing behaviour). The daemon restart reads the new
values and writes them to shared memory. The next `pi-start` call (from
the healer or from a manual restart) pushes the new values to the pi.

## 4. Order of work

Do the steps in this order. Each step is independently testable.

1. **`jackbridge-ctl` push (step 3.2).** This is a shell change only and
   does not require a rebuild. After the change, run `jackbridge-ctl restart`
   and check that `/etc/default/jackbridge` on the pi contains the Mac values.
   This immediately fixes the "reflash resets L and G" problem.

2. **Shared memory fields, protocol bump, and daemon publish (steps 3.3 to
   3.6).** Rebuild the daemon only. After the restart, run `just shm` and
   check that `NET_LATENCY` and `NET_RING` appear in the output.

3. **HAL reads shared memory for the latency model (step 3.7).** Rebuild
   the driver only. After the restart, run `just logs | grep latency` and
   check that the daemon and driver log lines show the same values.

4. **Settings UI (step 3.8).** This is a Swift change and does not require
   a C++ rebuild. After the change, open the Settings window, change L,
   click Apply, and verify that `config.plist` and `/etc/default/jackbridge`
   on the pi both contain the new value.

## 5. Constraints

**The constraint `G / 2 > L × period` must hold.** If it does not, the pi
xruns immediately. The Settings UI must validate this on Apply and must
refuse invalid combinations before writing `config.plist`.

**The plausibility check in the HAL (step 3.7) must treat zero as a
fallback.** A newly installed driver paired with an old daemon (before step
2) must still advertise a reasonable latency.

**The `push_pi_config` call must run before `systemctl start`.** If the
service starts before the file is written, it reads stale values.

**The pi `sudoers` must allow `pistomp` to run `sudo tee` without a
password.** The pi image already sets this up. Verify this before starting
step 1.
