# Codebase tour

A guided walk through the files that matter, what's in them, and what's safe to ignore.

## Top level

```
app/                      PiStompCompanion menu-bar app
jackbridge/daemon/        Source of truth for the userland process
jackbridge/driver/        Source of truth for the HAL bundle
jackbridge/shared/        Shared JackBridge IPC contract
jackbridge/tools/         Shm utilities and service controls
jackbridge/installer/     Package, LaunchAgents, and deployment helpers
jackbridge/pi/            Pi-side systemd service and helpers
README.md                 Product quick-start
LICENSE                   License
```

## `jackbridge/daemon/` — the JACK-side process

```
JackBridge.cpp       Main: opens shm, drives JACK client, and owns realtime transfer
JackBridge.h         IPC contract include
jackClient.cpp       Wraps jack_client_open and port registration
jackClient.hpp       Header for above
```

**`JackBridge.cpp` highlights:**
- The JACK callback is the realtime transfer path between JACK and shared memory.
- Startup validates the shared-memory protocol before entering the callback.
- The daemon exits on JACK shutdown so the LaunchAgent can restart it.

## `jackbridge/driver/` — the AudioServerPlugIn HAL bundle

```
JackBridgePlugIn.xcodeproj/      Xcode project. Deployment target 13.0.
JackBridge/Plug-In/
    SA_PlugIn.cpp                AudioServerPlugInDriverRef boilerplate
    SA_Device.cpp                The device: property tables and IO operations
    SA_Object.cpp                Base class for HAL objects
    JackBridge.h                 Shared IPC contract include
    Resources/                   Bundle resources
PublicUtility/                   Vendored Apple CoreAudio utility classes
```

**`SA_Device.cpp` highlights:**
- Fixed 48 kHz Float32 CoreAudio device with four inputs and two outputs.
- HAL timing and IO state are published to the shared-memory contract.
- The IO path is realtime-sensitive: no allocation, syscalls, or logging.

`PublicUtility/` contains the Apple CoreAudio utility wrappers used by the
HAL target.

## `jackbridge/tools/`

```
chkshm.c    Read and dump the shm region for debugging. Useful for verifying daemon is alive.
rmshm.c     Unlink stale shm regions.
jackbridge-ctl  Start, stop, restart, status, and log the Mac services.
```

The tools are single-file utilities or small shell controls for the JackBridge
engine.

## What to read first
If you're new to the codebase, in order:
1. `jackbridge/shared/JackBridge.h` — the IPC contract.
2. `jackbridge/daemon/JackBridge.cpp` — the JACK process callback.
3. `jackbridge/driver/JackBridge/Plug-In/SA_Device.cpp` — HAL IO operations.
4. `app/PiStompCompanion/StatusMonitor.swift` — the user-facing health model.
That's ~30% of the code and ~95% of what matters for the engine.
