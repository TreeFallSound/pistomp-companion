# First-run experience

## Goal

Make a new installation understandable without requiring users to read the
service architecture first.

## User flow

1. Install JACK2 as the documented prerequisite.
2. Install `PiStompCompanion-<version>.pkg`.
3. Open PiStomp Companion from `/Applications`.
4. Connect the direct Ethernet cable.
5. Enable Ethernet Audio Interface on the Pi.
6. Select JackBridge in the DAW once the Companion reports streaming.
7. Optionally enable Companion launch at login.

## Product behavior

- Treat the Companion as the normal control surface for status and start,
  stop, and restart actions.
- Keep `jackbridge-ctl` available as an advanced recovery escape hatch.
- Explain the required JACK2 dependency, wired-network requirement, and
  Pi-side on-demand service in the initial status/help surface.
- Make unavailable actions visibly explain why they are disabled rather than
  silently doing nothing.
- Do not auto-enable the Pi service or silently alter DAW device selection.

## Acceptance criteria

- A first-time user can identify the next action from the menu without
  consulting launchd documentation.
- The app clearly distinguishes “JACK2 missing”, “Pi not reachable”, “Pi
  reachable but audio disabled”, “linked but idle”, and “streaming”.
- Launch-at-login remains an explicit user choice and persists correctly.
- The service continues to function when the app is not running.
- All first-run copy matches the actual package and runtime behavior.

## Non-goals

No preferences window, DAW auto-configuration, or replacement for the Pi LCD
network control belongs in this pass.
