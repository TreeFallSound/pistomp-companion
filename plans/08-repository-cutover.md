# Repository cutover

## Goal

Make `treefallsound/pistomp-companion` the single public home and remove the
legacy JackRouter identity from active product surfaces.

## Work

- Keep the new repository’s top-level product framing in `README.md`.
- Preserve JackBridge as the internal engine, installed service, HAL bundle,
  and IPC namespace.
- Replace active documentation, workflow, release, and source-install links
  with `https://github.com/treefallsound/pistomp-companion`.
- Keep historical upstream references to `madhatter68/JackRouter` where they
  explain provenance; do not rewrite those as product names.
- Configure the new repository’s default branch, release workflow, issue
  templates, and package artifact paths.
- Add a short migration note to the old repository directing users to the new
  project, then make the old repository read-only or archive it.
- Tag the first release in the new repository only after packaging, hardware,
  and soak acceptance is complete.

## Acceptance criteria

- New contributors find the app, engine, build command, and release process
  from the repository root.
- No active install or release instruction points at the old repository.
- The old repository does not receive competing product commits.
- Release links, package names, and ownership consistently use
  `treefallsound/pistomp-companion` and `PiStomp Companion`.
- The service-only escape hatch remains documented as an implementation and
  recovery interface, not as a competing product identity.

## Compatibility rule

The first public package used the `com.jackbridge.*` namespace. The new package
must migrate those launchd labels and forget `com.jackbridge.pkg` during
installation. No new runtime component may retain the old namespace; these
identifiers are compatibility surfaces, not branding.
