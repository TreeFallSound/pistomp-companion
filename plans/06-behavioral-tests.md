# Behavioral tests

## Goal

Protect the Companion’s observable state model and failure handling with
small deterministic tests rather than testing implementation details.

## Test seams

Add testable seams around the current concrete monitors so tests can provide
snapshot, reachability, graph, and command results without starting real
services.

## Required cases

- Health precedence when protocol mismatch, heartbeat, graph, and driver
  status disagree.
- Heartbeat progress versus a stalled daemon.
- HAL read-head progress versus started-but-idle.
- Protocol version zero during startup and a mismatched nonzero version.
- Reachability success through `pistomp.local` and cached-IP fallback.
- Unreachable Pi with stale cached address.
- Diagnostics command success, nonzero exit, timeout, and missing executable.
- Shared-memory attach failure, wrong size, successful snapshot, and remap
  after object replacement.
- IPv4 and IPv6 endpoint formatting for MOD-UI and SSH URLs.

## Acceptance criteria

- Tests fail for each of the known plausible regressions: stale status,
  incorrect health precedence, uncapped diagnostics, and stale shm mapping.
- Tests are deterministic, isolated, and do not require JACK, a Pi, network
  access, or administrator privileges.
- The release build runs the tests in CI alongside the app compile check.
- Hardware-only behavior remains covered by the separate transition matrix and
  soak procedure rather than by brittle mocks.
