# Audio and service soak test

## Goal

Establish that the Companion integration remains stable during long-running
DAW use and service disruption.

## Procedure

- Run a real DAW capture/playback session through JackBridge for several hours,
  then a 24-hour run before release.
- Leave the Companion running and collect its diagnostics/log output at the
  beginning and end of the run.
- Exercise cable unplug/replug cycles during idle and active streaming.
- Kill and recover `jackd` while the DAW is open.
- Kill and recover `JackBridged` while the DAW is open.
- Restart the Pi-side recording service.
- Quit and relaunch the Companion while audio continues.
- Confirm that a Companion crash or forced termination does not interrupt the
  JackBridge services.

## Measurements

Record audible clicks/dropouts, DAW device disappearance and recovery, xrun
counts, duplicate JACK clients, service restart latency, and Companion state
convergence. Preserve `/tmp` service logs and the Companion diagnostics file
for any failure.

## Acceptance criteria

- The initial multi-hour run has no unexplained audio interruption.
- The 24-hour run has zero hangs and no accumulating duplicate netJACK2
  clients.
- Jackd and daemon failures recover through launchd without manual app restart.
- Cable and Pi service recovery return to streaming without stale status.
- Any remaining failure has a reproducible trigger and an owner before a
  release label is applied.

## Prerequisites

Complete the correctness work and hardware transition matrix first; otherwise
soak failures cannot be separated from known status/remapping defects.
