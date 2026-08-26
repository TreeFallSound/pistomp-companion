# Hardware transition validation

## Goal

Prove the user-visible state model against a real Mac, direct Ethernet cable,
and pi-Stomp rather than only against successful compilation.

## Test matrix

Exercise these transitions while the app is running:

1. App launch with no stack and no cable.
2. Cable insertion with the Pi reachable but no netJACK2 ports.
3. Pi-side Ethernet Audio enablement and JACK graph discovery.
4. Linked-but-idle state before CoreAudio IO starts.
5. Active DAW streaming.
6. Cable removal and reconnection.
7. `jackd` termination and launchd recovery.
8. `JackBridged` termination and launchd recovery.
9. `/JackBridge` removal and recreation.
10. Deliberate protocol mismatch.
11. Menu start, stop, and restart actions.
12. SSH and MOD-UI actions through the resolved Pi address.

## Evidence

For each transition capture the menu-bar icon, status line, reachability
address, JACK graph result, and relevant service logs. Record the time from
physical or process event to the displayed state. Confirm that quitting or
relaunching the app does not interrupt audio.

## Acceptance criteria

- Every matrix transition reaches the expected state without restarting the
  Companion.
- Reconnection does not leave duplicate netJACK2 clients or stale status.
- Protocol mismatch is clearly actionable and does not masquerade as a network
  failure.
- Service recovery works with the Companion closed.
- No audio interruption is caused by app polling, diagnostics, or relaunch.

## Prerequisites

A known-good arm64 JACK2 installation, current Pi-side service image, direct
wired link, a DAW capable of selecting JackBridge, and reproducible access to
`jackbridge-ctl` and the installed service logs.
