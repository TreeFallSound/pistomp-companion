# PiStomp Companion Settings Editor

## Purpose

Remove the need for Xcode during normal configuration.

Keep the current launchd service model.

Do not add a user control for JACK period values.

The startup coordinator shall discover the Pi audio values and pass the values to JACK.

## Problem

`Settings…` in `AppDelegate.swift` opens the system plist in an external editor.

The current plist path is `/Library/Application Support/JackBridge/config.plist`.

The current LaunchAgents watch this system path.

The route LaunchDaemon also watches this system path.

The route LaunchDaemon runs as `root`.

A settings window that writes only a home file will not update the route service unless the service reads the home file or receives a change signal.

## Evidence

| Source | Finding |
|---|---|
| `app/PiStompCompanion/AppDelegate.swift:201-205` | The app opens the system plist. |
| `jackbridge/installer/config.plist:6-11` | A plist change restarts the JackBridge agents. |
| `jackbridge/installer/launchagents/com.jackbridge.jackd.plist:40-44` | The JACK LaunchAgent watches the system plist. |
| `jackbridge/installer/launchagents/com.jackbridge.daemon.plist:33-36` | The daemon LaunchAgent watches the system plist. |
| `jackbridge/installer/launchdaemons/com.jackbridge.route.plist:25-31` | The root route service watches the system plist. |
| `jackbridge/installer/jackd-launch:103-162` | The wrapper reads the clock UID and period, then starts JACK. |
| `/usr/local/bin/jackd -d coreaudio --help` | The CoreAudio backend accepts `--rate`, `--period`, `--playback`, and related startup options. |
| `jackbridge/daemon/jackClient.cpp:81-93` | The daemon reads the active JACK rate and buffer size at startup. |
| `jackbridge/pi/README.md:68-74` | The Pi image uses 48 kHz and 128 frames by default. |
| `../pi-stomp/pistomp/alsa_pcm.py:18-40` | The Pi reads `rate` and `period_size` from ALSA `hw_params`. |
| `../pi-stomp/modalapi/ethernet/manager.py:89-111` | The Pi uses one hardware read for rate and period. |
| `../pi-stomp/ui/ethernet_menu.py:173-181` | The Pi displays the live rate and period. |
| `docs/idiosyncrasies.md:69-71` | The Pi and Mac periods must match. |
| `jackbridge/pi/README.md:70-72` | A Pi period change affects the complete Pi audio stack. |

## Product decisions

1. Use a native settings window.
2. Use explicit `Apply` and `Cancel` actions.
3. Keep editable user settings in the home directory.
4. Expose safe settings only.
5. Offer automatic clock selection and a CoreAudio device list.
6. Do not expose `PeriodFrames` in the settings window.
7. Do not expose `SampleRate` in the settings window.
8. Do not expose a period multiplier.
9. Use a bounded, non-interactive SSH probe for Pi timing data.
10. Use the last successful Pi timing data when the Pi is not available.
11. Keep launchd responsible for service life cycle.

## Home configuration

Use this path:

`~/Library/Application Support/JackBridge/config.plist`

The home plist shall contain only persistent user settings.

- `PiHostname`
- `ClockDeviceUID`
- `NetworkInterface`

The first version shall not expose `JitterFrames`.

The runtime shall keep one documented `JitterFrames` default.

The plist shall not contain these runtime-discovered values:

- `SampleRate`
- `PeriodFrames`

The plist shall not contain build identity values:

- `JackPrefix`

The package shall keep the JACK prefix in its installed runtime data.

The editor shall not allow a user to change the JACK prefix.

## Initial file creation

The installer shall create the home plist when the home plist does not exist.

The installer shall use the shipped default plist as the source.

The installer shall copy only persistent user settings to the home plist.

The installer shall not copy `SampleRate` or `PeriodFrames`.

The installer shall not replace an existing home plist.

The app shall create the home plist when the installer had no active console user.

The app shall set the home plist owner to the current user.

The app shall write the home plist with mode `0600`.

The installer or app shall create the parent directory with mode `0700`.

The initial file creation shall report a clear error when the home directory cannot be written.

## Service integration

The home plist becomes the source for persistent user settings.

The user LaunchAgents shall read the home plist.

The root route service shall read the active console user's home plist.

The installer shall generate absolute home paths in the LaunchAgent and route service configuration.

The generated paths shall not use an unresolved tilde.

The route service shall continue to use the current wired-interface detector.

The route service shall validate `NetworkInterface` before it uses the value.

The route service shall reject values that are not valid interface names.

The app shall request a service restart after a successful Apply.

The app shall not start a second service stack.

The service shall keep the current wired-network gate.

The service shall keep the current ownership checks for JACK.

The service shall keep the current restart and throttle behavior.

## Startup coordinator

The startup coordinator shall run inside the existing JACK launch path.

The coordinator shall perform these steps:

1. Read the home plist.
2. Resolve the Pi hostname.
3. Resolve the clock device UID.
4. Run the bounded Pi probe.
5. Parse the Pi sample rate.
6. Parse the Pi period.
7. Validate the values.
8. Save the last successful values in a user-owned runtime state file.
9. Start JACK with explicit startup options.
10. Start the JackBridge daemon through the existing LaunchAgent.

The runtime state file shall not be a plist.

The runtime state file shall contain the last successful Pi sample rate and period.

The runtime state file shall use an atomic replacement.

The runtime state file shall use mode `0600`.

The coordinator shall pass the values as direct process arguments.

The coordinator shall not construct a shell command string from user input.

The coordinator shall validate numeric values before it passes the values to JACK.

The coordinator shall pass equal Pi and Mac period values.

The coordinator shall use the cached values when the Pi probe fails.

The coordinator shall log that the cached values are stale.

The coordinator shall not start JACK when no valid live or cached values exist.

The coordinator shall return a non-zero status in that case.

The LaunchAgent `KeepAlive` setting shall retry the start.

The coordinator shall support explicit options for diagnostics and tests:

- `--rate <integer>`
- `--period <integer>`
- `--clock-device <uid>`

The service LaunchAgent shall use the normal no-argument path.

The no-argument path shall perform the probe and use the cache when required.

## Pi probe

Use `/usr/bin/ssh`.

Use the management hostname from `PiHostname`.

Use the `pistomp` account.

Use `BatchMode=yes`.

Use a connection timeout.

Use one connection attempt.

Read this fixed remote path:

`/proc/asound/card0/pcm0p/sub0/hw_params`

Read the file with a fixed remote `cat` command.

Parse `rate` on the Mac.

Parse `period_size` on the Mac.

Reject the result when the file reports `closed`.

Reject the result when a field is missing.

Reject the result when a field is not an integer.

Reject sample rates other than the supported rate.

Reject unsupported period values.

Treat SSH authentication failure as an unavailable probe.

Treat a timeout as an unavailable probe.

Do not prompt for a password from the settings window.

The settings window shall show probe status and probe age.

## Settings window

Replace `openSettings` with a window controller.

Host the view in the existing AppKit application.

Use SwiftUI only for the view layer if it does not change the app life cycle.

Use the existing AppKit window pattern for window ownership and activation.

The window shall contain these sections:

### Connection

- Pi hostname text field.
- Automatic network interface option.
- Network interface list.
- Automatic clock device option.
- CoreAudio output device list.

Store a clock device UID.

Do not store the device display name.

### Audio status

Show the Pi sample rate.

Show the Pi period.

Show the source of the values:

- Live probe.
- Cached value.
- No value.

Show a stale-data message for cached values.

Do not make the rate or period editable.

### Advanced

The first version shall not expose `JitterFrames`.

The window shall not show `JackPrefix`.

The window shall not show unsupported plist keys.

### Actions

- `Apply`.
- `Cancel`.
- `Reset to Defaults`.
- `Probe Pi Now`.

`Apply` shall validate all fields before it writes.

`Apply` shall write the home plist once.

`Apply` shall restart the existing service stack once.

`Cancel` shall discard all unsaved changes.

`Reset to Defaults` shall use the same defaults as the runtime.

A restart warning shall appear before `Apply` completes.

The window shall remain open when a write or restart fails.

## Configuration contract cleanup

Remove `PeriodFrames` from the persistent configuration contract.

Remove `SampleRate` from the persistent configuration contract.

Remove dead settings from the editor and the template.

Keep a single documented default for every remaining setting.

Align the driver and daemon fallback values for `JitterFrames`.

Update comments that name the system plist path.

Update `JackTools` to read the home plist.

Update the shell readers to use the resolved home path.

Update the C++ readers to use the resolved home path.

Keep `JackPrefix` in the package runtime path.

Do not change the shared-memory protocol.

Do not change the JACK graph contract.

## Error behavior

Show a read error when the home plist is malformed.

Show a write error when the home plist cannot be written.

Show a probe error without blocking the settings window.

Show a service restart error when the control command fails.

Do not report `Apply` success before the home write succeeds.

Do not delete a valid last-known runtime state after a failed probe.

Keep the previous active service when validation fails.

## Verification plan

### Unit checks

Test home plist creation from the shipped default.

Test that an existing home plist is not replaced.

Test preservation of unknown keys that are not runtime keys.

Test removal of `SampleRate` and `PeriodFrames` from the created home plist.
Test malformed plist handling.

Test field validation.

Test atomic home plist replacement.

Test probe parsing for valid `hw_params` output.

Test probe parsing for `closed` output.

Test probe parsing for missing fields.

Test probe timeout handling.

Test cached-value use after probe failure.

Test refusal to start without live or cached timing data.

Test equal period arguments for Pi and Mac.

Test explicit coordinator startup options.

### Service checks

Run the coordinator with a fake probe result.

Confirm the coordinator builds direct JACK arguments.

Confirm the coordinator does not build a shell command string.

Confirm the existing LaunchAgents still own the processes.

Confirm a home plist change causes one service restart.

Confirm a network interface change causes one route refresh.

Confirm a failed Apply does not change the running service.

### Hardware checks

Connect the Pi through the management network.

Start the Pi Ethernet Audio service.

Read the Pi rate and period through SSH.

Start the Mac service.

Confirm the Mac JACK period equals the Pi JACK period.

Confirm the status line reports the active rate and period.

Disconnect the Pi.

Confirm the cached values appear with a stale-data message.

Confirm a first start with no valid timing data does not start a mismatched JACK server.

Change the Pi period outside the app.

Confirm the next live probe updates the Mac startup period.

## Delivery limits

This plan does not change the Pi JACK period.

This plan does not add remote Pi configuration writes.

This plan does not add a Pi API.

This plan does not add a second service manager.

This plan does not expose unsupported plist keys.

This plan does not change the audio protocol.
