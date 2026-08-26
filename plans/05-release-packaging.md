# Release packaging

## Goal

Produce one trusted installer whose user-facing product is PiStomp Companion
and whose payload contains the JackBridge engine it needs.

## Current product boundary

The package installs:

- `PiStompCompanion.app` in `/Applications`.
- The JackBridge HAL bundle.
- `JackBridged`, helpers, `jackbridge-ctl`, config, and launch services.
- The route LaunchDaemon and per-user LaunchAgents.

JACK2 remains a separately installed prerequisite for the first release. Keep
`com.jackbridge.pkg` as the package identifier so upgrades remain compatible.

## Release work

- Sign the app, HAL bundle, daemon, and helper binaries with Developer ID
  Application.
- Sign the outer product with Developer ID Installer.
- Notarize and staple the package.
- Verify installation on a clean arm64 Mac, including Gatekeeper behavior.
- Verify upgrade behavior over an existing JackBridge installation.
- Verify config preservation, launchd bootstrap, `coreaudiod` restart, and
  package recovery paths.
- Publish the JACK2 prerequisite and Companion package together, with explicit
  installation order.
- Confirm the release workflow uploads the artifact from
  `jackbridge/installer/build/`.

## Acceptance criteria

- A clean Mac can install the signed package without the unsigned-package
  workaround.
- The HAL device appears after installation and service startup succeeds.
- Reinstalling preserves a hand-edited config.
- The package contains the app, HAL, daemon, helpers, and service definitions.
- A failed app launch does not prevent the audio services from running.
- The release artifact and README use `treefallsound/pistomp-companion` and
  `PiStompCompanion-<version>.pkg` consistently.
