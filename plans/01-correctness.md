# Companion correctness

## Goal

Make the Companion reliable when the JackBridge stack is starting, stopping,
restarting, or partially connected. The app must never hang diagnostics, show
mixed shared-memory state, or remain attached to a deleted shm object.

## Scope

- Serialize `StatusMonitor.State` updates. `pollShm()` currently runs on
  `shmQueue` while `recompute()` consumes and mutates the same state on the
  main queue. Make one queue authoritative and deliver immutable snapshots to
  the UI.
- Make every diagnostics subprocess bounded. Add a watchdog to both JACK
  probes and generic commands; escalate from graceful termination to a hard
  kill, then collect output without blocking indefinitely.
- Detect `/JackBridge` recreation. Track the shm object identity and remap
  after `shm_unlink`, daemon recovery, or package upgrade. Preserve a useful
  error state during the transition.
- Centralize runtime JACK tool discovery. The app, installer, and runtime
  wrappers must agree on the supported JACK prefix instead of silently mixing
  `/usr/local` and alternate prefixes.

## Acceptance criteria

- Thread Sanitizer or equivalent concurrency instrumentation reports no race
  between shm polling and UI recomputation.
- A deliberately hung `jack_lsp` or probe exits the diagnostics operation
  within its documented budget and produces an exit/timeout record.
- Removing and recreating `/JackBridge` causes the app to display fresh values
  without relaunching the app.
- A single documented JACK prefix is used consistently by build, package, and
  Companion runtime code.
- Existing health precedence remains: protocol mismatch is red; streaming,
  idle, unreachable, and stack-down states remain distinguishable.

## Constraints

The Companion remains read-only with respect to the shm region and must not
add work to either realtime audio path. Keep the service lifecycle in launchd
and `jackbridge-ctl`; the app only observes and invokes controls.
