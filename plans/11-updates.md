# Companion Updates

## Status

Proposed. This plan covers a native **Updates** menu in PiStomp Companion,
with availability notification, candidate-version inspection, changelog
viewing, and one-click apt installation on the connected pi-Stomp.

The feature updates packages from the pi's configured TreeFallSound apt
repository. It does not update the macOS Companion application itself. A
Companion release-update check would be a separate feature backed by GitHub
Releases.

## Goal

A user should be able to see and install supported pi-Stomp OTA package
updates without opening SSH or a terminal:

```text
Updates (5)
────────────
jackbridge 0.3.0 → 0.3.1
mod-ui 1.4.2 → 1.4.3
pistomp-recovery 0.6.0 → 0.6.1
...
```

The menu must make update availability visible without opening the menu. The
update marker is a small blue dot at the **top-left** of the tray icon. The
existing health marker remains at the lower-right and keeps its current
meaning.

## Product decisions

1. **Updates is a submenu in the SSH/Deploy group.**
2. The submenu title includes the known update count: `Updates (5)`. With no
   updates it is `Updates`; while checking it may use `Updates (checking…)`.
3. The first row is `Update All (N)`, followed by a separator, followed by
   individual package rows.
4. Opening Updates refreshes the result when the last successful check is more
   than five minutes old. A refresh never runs on the main queue.
5. A background check runs after app launch and periodically thereafter, using
   a bounded interval. It must not run `apt-get update` on every menu hover.
6. A transient check failure does not clear a previously known update marker.
   A successful check is the only operation that may change availability to
   `current`.
7. Selecting an individual package opens a native detail window showing the
   installed and candidate versions, package description, changelog, and an
   `Install` button. The package is not installed merely by opening its
   detail view.
8. `Update All (N)` confirms once, then installs all eligible packages.
9. Installing or updating audio packages may interrupt audio. The confirmation
   and progress UI must say so.
10. `pi-stomp` is excluded from update installation while
    `/opt/pistomp/pi-stomp/.git/EXPANDED` exists. Git deployment owns that tree.
    It may be listed as blocked with an explanation, but it must not be part of
    `Update All`.
11. The Companion does not hard-code the apt URL. Package discovery is based on
    apt repository metadata and the `Origin: pistomp` identity already used by
    the device image.

## Existing implementation evidence

| Source | Relevant behavior |
|---|---|
| `app/PiStompCompanion/StatusItemController.swift` | The base icon is a template image and health is an overlay badge. Add the update dot to the overlay, at top-left, without recoloring the base image. |
| `app/PiStompCompanion/ProgressWindowController.swift` | Existing native progress window with indeterminate startup, determinate progress, and status text. Extend it for update phases and failure completion. |
| `app/PiStompCompanion/ProcessRunner.swift` | Bounded subprocess execution, but output is collected only after exit. Live apt progress requires a streaming process/SSH path. |
| `app/PiStompCompanion/AppDelegate.swift` | Owns the tray menu, reachability state, SSH/Deploy actions, and main-queue rendering. Add update state here or behind a dedicated client. |
| `../pistomp-recovery/src/pistomp_recovery/packages/manager.py` | Existing apt implementation: source-restricted refresh, package discovery by `Origin`, installed/candidate comparison, install, and package detail. |
| `../pistomp-recovery/src/pistomp_recovery/app.py` | Existing update UX: `Update All`, package rows, expanded `pi-stomp` exclusion, package detail, progress, and service restart sequencing. Reuse semantics, not the Pi LCD UI. |
| `../pistomp-recovery/docs/CHANGELOG_VIEWER_PLAN.md` | Documents why candidate changelogs are currently unavailable or may incorrectly show the installed version. |
| `../pi-gen-pistomp/config.sh` | Defines the TreeFallSound pi-Stomp source and the `pistomp-recovery` package sources. |

## Architecture

### Device-side update protocol

Do not make Swift parse human-oriented `apt` output. Add a small device-side
command surface, preferably to the installed `pistomp-recovery` package, that
uses the existing package-manager logic and emits a stable line protocol or
NDJSON.

Required operations:

```text
check       refresh the selected apt sources and emit update records
changelog   emit the candidate package changelog
install     download and install named packages, emitting progress records
```

An update record contains at least:

```json
{
  "name": "mod-ui",
  "installed": "1.4.2",
  "candidate": "1.4.3",
  "origin": "pistomp",
  "eligible": true
}
```

Progress records contain a phase, completion fraction where meaningful, and a
human-readable status. The command must return a nonzero result with a useful
error record when apt, sudo, repository access, package locks, or service
restarts fail.

The helper should use the same source restriction as
`AptManager.sync_db_restricted()` in
`../pistomp-recovery/src/pistomp_recovery/packages/manager.py`. This avoids
refreshing slow or unrelated Debian/Raspberry Pi mirrors and supports the
stable and testing pi-gen suites configured on the device.

The helper must discover packages by Release metadata (`Origin: pistomp`),
matching `AptManager.discover_packages()`. `apt list --upgradable` alone is
not sufficient because it includes packages from unrelated apt origins.

### Companion-side client

Add a dedicated remote update client rather than embedding apt shell strings in
`AppDelegate`.

Responsibilities:

- execute the device helper over the existing SSH identity and configured
  hostname;
- stream progress without blocking AppKit;
- decode update and progress records;
- serialize checks and installs so two apt operations cannot overlap;
- expose cancellation only for the refresh phase unless apt installation can
  be cancelled safely;
- retain the last successful update result and its timestamp;
- report authentication, connectivity, sudo, apt-lock, and parse failures as
  distinct states.

Extend `ProcessRunner` with a bounded streaming variant, or create a focused
SSH runner. The existing completion-only runner cannot drive an honest live
progress dialog for `apt-get`.

### Availability state

Maintain update state independently from audio health:

```text
unknown → checking → current
                  ↘ available(count)
                  ↘ failed
```

State invariants:

- `failed` is not equivalent to `current`;
- a failed refresh does not erase a prior `available(count)` state;
- `available(count)` includes only eligible `Origin: pistomp` packages;
- the count excludes expanded `pi-stomp` when it cannot be installed;
- a successful install is followed by a fresh check before clearing the dot.

The update count and badge are main-queue-owned. Background SSH work may only
publish immutable results back to the main queue.

## Companion UI changes

### Menu

Add an `Updates` submenu next to `Deploy`.

When updates exist:

```text
Updates (5)
  Update All (5)
  ─────────────
  jackbridge 0.3.0 → 0.3.1
  mod-ui 1.4.2 → 1.4.3
  pistomp-recovery 0.6.0 → 0.6.1
```

When checking, show a disabled `Checking for updates…` row. On failure, show a
disabled, specific row such as `Update check failed` and retain any prior
availability marker. With no eligible updates, show a disabled `No updates
available` row.

Individual rows open the detail window. `Update All` is disabled while a check
or installation is active.

### Icon

Change `StatusItemController` so it accepts both health state and update
availability. Draw the update indicator as a blue filled dot at the icon's
top-left. Keep the health badge at the lower-right with its existing colors.

Do not encode update availability by dimming or recoloring the template image.
Do not replace a red/amber health state with the update dot. Both facts must
remain visible simultaneously.

The status item's accessibility description or tooltip should expose the same
information in text, for users who cannot distinguish the dots.

### Detail and changelog window

Add a native scrollable detail window. It should show:

- package name;
- installed and candidate versions;
- apt package description;
- a `Changelog` section or button;
- `Install`.

Changelog loading is asynchronous and must show `No changelog available` rather
than silently substituting the installed package's changelog. The viewer must
handle long entries with scrolling and preserve the package version context.

### Progress

Reuse `ProgressWindowController` after extending it to support:

- source refresh;
- package download;
- package installation;
- affected-service restart;
- success completion;
- failure completion with an actionable error.

The window must remain visible long enough for users to read failure output.
The app must refresh availability after success and must not clear the update
dot merely because installation started.

## Required `pi-gen-pistomp` work

The Companion depends on the following repository-side changes. These are
required for accurate candidate changelogs and trustworthy apt operations,
not optional UI polish.

### 1. Preserve `.changes` files in release assets

**File:** `../pi-gen-pistomp/.github/workflows/build-deb.yml`

Current behavior:

- the build produces a `.deb`;
- the release asset assembly includes the `.deb` and optional `.built-sha`;
- the changelog text is copied into the GitHub Release body;
- the `.changes` file is not retained as a release asset.

Change the asset assembly to include the matching `.changes` file for every
`dpkg-buildpackage` package. Validate that every referenced `.deb` and source
metadata file is present before publishing. Keep `.built-sha` behavior
unchanged.

### 2. Make every published package produce usable changelog metadata

**Files:** `../pi-gen-pistomp/debpkgs/*/build.sh`

Most packages already use `dpkg-buildpackage`. The packages currently built
with `dpkg-deb --build`—including `lcd-splash` and
`libfluidsynth2-compat`, as documented in the recovery changelog plan—must be
migrated or otherwise given equivalent versioned changelog metadata.

The acceptance target is one consistent package publication path: a candidate
package's changelog can be resolved without downloading the `.deb` merely to
inspect it.

### 3. Publish `.changes` through reprepro

**File:** `../pi-gen-pistomp/.github/workflows/publish-apt-repo.yml`

Current behavior downloads only `.deb` release assets and calls:

```sh
reprepro -b . includedeb trixie <deb>
```

Change the workflow to download the matching `.changes` assets and use
`reprepro include` so the repository contains the canonical changelog-related
metadata. Apply the same behavior to `trixie-testing`. Preserve duplicate
name/version protection and the existing release-channel routing.

Update the workflow checks so a missing `.changes` asset fails loudly rather
than silently publishing a package that has no candidate changelog.

### 4. Sign the apt repository

The current device source uses `trusted=yes`:

- `../pi-gen-pistomp/stage2/00-dummy-packages/01-run.sh`
- `../pi-gen-pistomp/scripts/migrate-apt-repo.sh`

The current publishing workflow also creates unsigned repository metadata.
Implement the signing work described in
`../pistomp-recovery/docs/CHANGELOG_VIEWER_PLAN.md` and
`../pi-gen-pistomp/docs/OTA.md`:

1. generate and protect a project-owned archive signing key;
2. install the public keyring in the image from
   `stage2/05-pistomp/01-run.sh`;
3. add `SignWith:` to the generated `conf/distributions` in
   `publish-apt-repo.yml`;
4. publish signed `InRelease`/`Release.gpg` metadata for both suites;
5. replace `trusted=yes` with `signed-by=/usr/share/keyrings/...` in
   `stage2/00-dummy-packages/01-run.sh`;
6. provide a transition path for existing devices before removing
   `trusted=yes` from `scripts/migrate-apt-repo.sh`.

Do not switch existing devices to an unsigned or unverifiable source as an
intermediate state. The keyring must arrive before the source line requires
it.

### 5. Preserve repository identity

Keep the existing `Origin: pistomp` and `Label: pistomp` metadata in
`publish-apt-repo.yml`. The Companion and recovery package discovery should
continue using the origin identity rather than matching a URL string.

Stable and testing suites must remain distinguishable. A testing device may
see both suites, while a stable device must not receive testing-only packages.

### 6. Update pi-gen OTA documentation

**File:** `../pi-gen-pistomp/docs/OTA.md`

Document:

- the signed source-line format;
- keyring installation and rotation;
- `.changes` publication;
- the `reprepro include` flow;
- the existing-device transition procedure;
- how to verify that a candidate changelog is available.

## Required `pistomp-recovery` work

The recovery repository is the source of the existing package semantics and
has its own changelog-viewer plan. Coordinate rather than duplicate behavior:

- extend the package backend/CLI with the structured device protocol needed by
  Companion;
- complete the candidate changelog implementation in
  `../pistomp-recovery/docs/CHANGELOG_VIEWER_PLAN.md`;
- keep `AptManager`'s `Origin: pistomp` discovery and source-restricted refresh;
- keep expanded `pi-stomp` excluded from apt installation;
- share package-to-service restart mapping between recovery and the helper;
- add tests for candidate version parsing, origin filtering, changelog
  unavailable behavior, and update-all exclusion.

The Companion should not import or depend on the recovery UI. It may depend on
the installed helper protocol supplied by the recovery package.

## Verification plan

### pi-gen repository

1. Build a representative `dpkg-buildpackage` package and verify its `.changes`
   file is included in the GitHub Release assets.
2. Run the publish workflow against a test repository and verify both suites
   contain the `.deb`, `.changes`-derived metadata, `InRelease`, and
   `Release.gpg`.
3. Install the public keyring into a test image and run apt refresh with
   `signed-by`; verify unsigned or wrongly signed metadata is rejected.
4. Verify stable and testing source configuration separately.
5. Run `apt-get changelog <package>` against a candidate newer than the
   installed version and verify it reports the candidate's entries.

### Device helper

1. Verify only `Origin: pistomp` packages are returned.
2. Verify a new, not-installed package is represented correctly.
3. Verify expanded `pi-stomp` is reported as blocked and excluded from
   Update All.
4. Verify apt lock, network, sudo, missing changelog, and failed service
   restart errors are structured and nonzero.
5. Verify progress records are ordered and installation completion is emitted
   only after affected services are handled.

### Companion

1. No updates: no top-left dot and no count suffix.
2. Five updates: blue top-left dot, `Updates (5)`, `Update All (5)`, separator,
   then five eligible package rows.
3. Health degraded plus updates: both health and update indicators remain
   visible.
4. Failed refresh after a known update: dot and prior count remain; menu shows
   the refresh failure.
5. Successful install followed by refresh: count and dot clear only when no
   eligible candidates remain.
6. Individual package detail shows installed/candidate versions and the
   candidate changelog; Install produces live progress and service status.
7. Update All installs the selected eligible set once and does not include
   expanded `pi-stomp`.
8. Repeated submenu opening does not start overlapping apt operations.
9. Companion remains responsive while SSH, apt refresh, changelog retrieval,
   or installation is active.
10. Replacing the `pistomp-recovery` package itself does not leave the UI in a
    false “current” state if the post-install refresh fails.

## Rollout order

1. Finish and verify signed apt metadata and `.changes` publication in
   `pi-gen-pistomp`.
2. Add/verify the structured update helper in `pistomp-recovery`.
3. Deploy the helper and repository changes to a test pi image.
4. Implement the Companion remote client and availability state.
5. Implement the menu, top-left notification dot, detail/changelog window,
   and progress flow.
6. Exercise single-package and Update All installs on a test device.
7. Document recovery behavior and release the Companion package.

## Non-goals

- Updating the macOS Companion from the pi apt repository.
- Updating arbitrary Debian/Raspberry Pi OS packages.
- Installing a candidate package before the user selects Install or Update All.
- Replacing the pi's recovery UI with a macOS-only source of package truth.
- Silently bypassing apt signatures, package locks, service failures, or the
  expanded Git ownership rule.
