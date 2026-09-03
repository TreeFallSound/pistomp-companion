# Bug Reporter and SSH Drive

## Status

Proposed. This plan adds persistent crash reports on the pi-Stomp and a
manually controlled, user-level SSH filesystem mount in PiStomp Companion.

The user experience is:

```text
SSH Drive                 ✓
Folders >
  Home
  pi-stomp
  pedalboards
  crash logs
```

`SSH Drive` is never started automatically. `Folders` entries are available
only while the drive is mounted. The SSH Drive item is disabled when the pi is
not available, except that an already-mounted drive remains stoppable if the
pi disappears.

## Goal

Every crash of a managed pi-Stomp service should leave a bounded, readable
record on the SD card. A user should be able to mount the pi's filesystem as
the unprivileged `pistomp` account and open the relevant folders in Finder,
including the crash-report directory.

The drive exposes the remote filesystem root (`/`), but the SSH server and
SFTP process run as `pistomp`. This is a root view, not a root-privileged
session: Unix permissions and the account's existing privileges remain the
security boundary.

## Research decision: macOS SSHFS stack

### Recommendation

Use the upstream **SSHFS userland client with macFUSE** as the supported
baseline, detected at runtime rather than bundled into the Companion package.

- Upstream SSHFS is GPLv2 and has an official macOS package. The macFUSE wiki
  lists SSHFS 3.7.5 for Apple Silicon and Intel, macOS 10.9+, and macFUSE
  4.10+; it also documents signed installer packages.
- The upstream SSHFS README explicitly recommends running as a regular user,
  which matches the required `pistomp`-only remote access model.
- The current project supports macOS 13+, so this path does not require a
  macOS 26-only API or a raised deployment target.

The complete stack is not entirely open source: macFUSE publishes its
libraries/framework, while its repository states that other components such
as the kernel extension are closed source. That limitation must be explicit
in the installation documentation.

### Why not make FUSE-T the default

FUSE-T is technically attractive: it is kext-less, uses NFS/SMB/FSKit
backends, and offers an SSHFS package. However, its published `License.txt`
says commercial use or bundling requires a commercial license. The project
cannot be treated as a drop-in open-source dependency for a distributable
Companion without first obtaining and recording that license.

FUSE-T can be a future optional backend after a license decision. It must not
be silently selected merely because it avoids a kernel extension.

### Runtime dependency behavior

Do not install either FUSE implementation from inside the app. Installing a
filesystem driver requires user consent, system-extension/security approval,
and separate licensing/release handling.

At startup, resolve `sshfs` from known executable locations and the user's
PATH. Prefer a supported version/backend and record the detected version in
app diagnostics. If the pi is available but no supported local SSHFS client is
installed, selecting `SSH Drive` shows a precise installation message and
links to the official macFUSE/SSHFS sources; it does not fail silently.

The runtime resolver must support Apple Silicon Homebrew (`/opt/homebrew`)
and the official `/usr/local` installer without assuming either one. Do not
use `allow_other`: that would broaden access to other local users and is not
needed for opening Finder as the current user.

## Existing implementation evidence

| Source | Relevant behavior |
|---|---|
| `app/PiStompCompanion/AppDelegate.swift` | Builds the tray menu, gates SSH/Deploy on reachability, owns application termination, and already opens Terminal for SSH. Add the SSH Drive and Folders entries here or delegate to a controller. |
| `app/PiStompCompanion/ProcessRunner.swift` | Bounded completion-oriented subprocess runner. A long-lived SSHFS process needs a dedicated lifecycle controller, not a blocking `run` call. |
| `app/PiStompCompanion/StatusMonitor.swift` / `ReachabilityMonitor.swift` | Existing pi reachability state is the menu's nonblocking availability signal. Use it to disable starting the drive. |
| `../pistomp-recovery/src/pistomp_recovery/service.py` | Current crash diagnosis identifies systemd `Result` failures and captures up to 100 journal lines only when recovery starts. It is not persistent per-crash storage. |
| `../pistomp-recovery/src/pistomp_recovery/packages/health.py` | Reads service state and journal through bounded `systemctl`/`journalctl` calls as the `pistomp` user with sudo. |
| `../pistomp-recovery/src/pistomp_recovery/constants.py` | Defines the managed service list, pi-Stomp source paths, and recovery data directory conventions. |
| `../pi-gen-pistomp/debpkgs/*/debian/*.service` | Managed service units already use `OnFailure=pistomp-recovery.service`; the existing handoff is the crash-report trigger. |
| `../pi-gen-pistomp/debpkgs/pistomp-recovery/debian/pistomp-recovery.pistomp-recovery.service` | Recovery runs as `pistomp`, conflicts with the main app, and is the existing crash-loop handoff target. |

## Crash reporter design

### Event boundary

Persist one report for each crash-recovery handoff—the same event that causes
`pistomp-recovery` to show the BSOD/crash screen. Do not attempt to record every
short-lived restart attempt separately. The existing service units already
route failures through `OnFailure=pistomp-recovery.service`; that existing
handoff is the trigger and must remain the only trigger.

`__main__.py` already calls `diagnose_crash()` before recovery stops or resets
anything. Immediately after that diagnosis, and before constructing the app,
write one report when the result is `BootMode.CRASH_RECOVERY`. A user-selected
recovery boot (`USER_RECOVERY`) must not create a crash report.

The report should include the primary `failed_service`, all service states and
systemd `Result` values returned in `CrashInfo`, and the bounded journal for
the primary failed service. If several managed services report a failure in
the same handoff, retain their metadata in the same report instead of
pretending they were separate crash events. This matches what the LCD shows
and avoids duplicate reports when several `OnFailure` edges converge on one
recovery service activation.

Do not add a second `OnFailure` hook, a templated capture service, or any new
systemd failure chain. Such a hook would duplicate the existing recovery
trigger and would race the recovery process that already snapshots the crash.

Implement the writer in the `pistomp-recovery` source package so it consumes
the existing `CrashInfo` and journal data. Package it through
`../pi-gen-pistomp/debpkgs/pistomp-recovery/`; do not copy a second Python
implementation into package maintainer scripts.

### Report location and ownership

Use a persistent, user-readable directory on the SD card:

```text
/home/pistomp/.pistomp-recovery/crash-logs/
```

Create it with owner `pistomp:pistomp` and restrictive directory permissions
that still allow the account to browse it. Reports should be readable by the
`pistomp` account because that is the account used by SSHFS. Do not expose
journal access or sudo through the mount.

Use one immutable file per recovery crash event. A human-readable UTC
timestamp and a unique identifier may be included in the filename, but the
filename is not the ordering authority. Write to a same-directory temporary
file, flush it, and rename it into place. A partial file after a power loss
must never look like a complete crash report.

### Report contents

Each report is plain text so it is useful in Finder, Terminal, and simple
support workflows. Include:

```text
pi-Stomp crash report
Timestamp: ...
Unit: mod-ala-pi-stomp.service
Invocation ID: ...
Boot ID: ...
ActiveState: ...
SubState: ...
Result: exit-code
ExecMainCode: ...
ExecMainStatus: ...
MainPID: ...
NRestarts: ...

--- service journal ---
...
```

Collect the implicated service's recent journal, preferably narrowed to the
failed systemd invocation when the journal exposes its invocation ID. Use a
bounded fallback of recent lines for older images. Preserve timestamps and
priority in the persisted report; the current LCD crash view may continue to
use its short journal presentation.

Include service-result metadata even when the journal is empty. A crash report
with no journal is still evidence of the failed unit and systemd result. Do
not dump the entire journal, process environments, private keys, or unrelated
units into the report.

If a coredump exists, include bounded identifying metadata only; do not copy
core files into the crash directory. The 50 MB budget is for text reports and
must not become a coredump retention policy.
### Rotation: 50 MiB hard ceiling

Define one named constant for the total crash-report budget:

```text
CRASH_LOG_BUDGET_BYTES = 50 * 1024 * 1024
```

Rotation is classic size-bounded retention with event files:

1. acquire an exclusive lock in the crash directory;
2. write and atomically install the new report;
3. enumerate only valid report filenames;
4. obtain each file's filesystem creation/birth time;
5. calculate total bytes;
6. delete the oldest reports by creation time until the total is at or below
   50 MiB;
7. release the lock.

Creation time is the only ordering authority. Do not use filename timestamps,
mtime, ctime, directory enumeration order, or a defensive fallback. The
supported pi-gen image has an ext4 root filesystem: `export-image/prerun.sh`
creates it with `mkfs.ext4`, and the image mounts `/` as ext4. Use Linux
`statx(2)` and require the returned `STATX_BTIME` bit for this report
directory.

Missing birth time is therefore an image/filesystem validation failure, not a
normal runtime branch. Fail the image/package check before release rather than
guessing an order or silently weakening the 50 MiB retention guarantee.

Renaming, rewriting, or changing mtime on an existing report must not change
its retention age. A copied report has the creation time assigned by the
destination filesystem and is treated accordingly; the writer must honour the
metadata actually present on disk.

Never delete the newest report to make room for older reports. If a single
report exceeds the budget, truncate its journal section with an explicit
marker before installation so the directory can still satisfy the hard
ceiling. Locking and atomic rename must make concurrent writers safe.

The lock file itself is not counted as a report. Temporary files are removed
on startup and ignored by the enumerator. A failed rotation must not delete
reports speculatively; report the error to the journal and leave the newest
complete file intact where possible.

### Recovery LCD behavior

Keep the existing BSOD/crash screen and its fullscreen log viewer. Change its
source of data only as needed so the first recovery view remains immediate:

- `diagnose_crash()` still snapshots the triggering service and current journal
  before service state is changed;
- the crash writer persists that snapshot during the existing recovery startup
  before the LCD app can reset or alter useful systemd state;
- the LCD may show a short live journal tail while the mounted crash report
  provides the complete bounded event record.

If persistence fails, the LCD should say that the crash report could not be
saved; it must not claim that no crash occurred.

## Companion SSH Drive design

### Controller and state

Add a dedicated `SSHDriveController` that owns the long-lived `sshfs`
`Process` and mount lifecycle. It must publish state on the main queue:

```text
stopped
starting
mounted
stopping
failed(error)
```

Invariants:

- no SSHFS process starts during app launch;
- at most one SSHFS process and one mountpoint exist;
- checked means the mount process is alive and the mountpoint passed a local
  mounted-filesystem check;
- a process exit clears the checked state and disables Folders;
- stopping is idempotent and safe after a network loss;
- app termination waits for the drive to unmount before exiting, subject to a
  bounded timeout;
- no local `sudo` is used for starting, stopping, or opening folders.

The controller should reconcile a stale mountpoint at startup. It may remove
an empty app-owned mountpoint directory, but it must never unmount an
unrelated filesystem. Identify the mount by its app-owned path and expected
volume identity before calling `umount`.

### Mount command

Mount the remote filesystem root with the SSHFS client as the current macOS
user:

```text
sshfs pistomp@<configured-host>:/ <app-owned-local-mountpoint>
```

Use the existing SSH key and host configuration. Batch mode should be used so
a menu click cannot hang on a password prompt. Surface authentication failure
as an alert that points to the existing SSH key installer/SSH menu path.

Use only options validated against the chosen SSHFS/macFUSE version. The
initial option set should favor safe recovery over aggressive caching:

- explicit volume name;
- bounded keepalive/reconnect behavior;
- no `allow_other`;
- no local root requirement;
- no password prompt from a hidden process;
- a bounded startup watchdog.

Do not promise that reconnect preserves every open Finder operation. If the
SSHFS process exits or the mount becomes stale, mark the drive stopped/failed
and let the user explicitly start it again.

Use an app-owned mountpoint under the user's Application Support directory,
for example:

```text
~/Library/Application Support/PiStompCompanion/Mounts/pi-Stomp
```

Create the parent and mountpoint with the current user's ownership. Set an
SSHFS volume name such as `pi-Stomp (pistomp.local)` so Finder identifies it
clearly. Keep the exact path centralized in the controller; Folders must not
reconstruct it independently.

### Start/stop menu item

Add one checked menu item:

```text
SSH Drive
```

The checkmark reflects controller state. It is unchecked while stopped,
starting failure, or after unexpected process exit.

- Disable starting when `lastState.piReachable` is false.
- If already mounted and the pi becomes unreachable, keep Stop available so
  the user cannot be trapped with an orphaned mount.
- Selecting an unchecked item starts the drive.
- Selecting a checked item stops it.
- Never auto-remount after a network transition.
- Show a short status message for starting, mounted, stopping, and failure.

The existing reachability probe is a gate, not proof that SSH authentication
will succeed. A reachable host with a missing/rejected key remains selectable
and produces a specific failure path.

### Folders submenu

Add a `Folders` submenu adjacent to `SSH Drive`. Its rows open local URLs
inside the mounted remote root via `NSWorkspace`:

| Menu row | Remote path |
|---|---|
| `Home` | `/home/pistomp` |
| `pi-stomp` | `/home/pistomp/pi-stomp` |
| `pedalboards` | `/home/pistomp/data/.pedalboards` |
| `crash logs` | `/home/pistomp/.pistomp-recovery/crash-logs` |

Use the actual mountpoint plus each remote-relative path. Do not use SFTP or
SSH commands for these actions; Finder must be browsing the same mounted
filesystem the user can inspect.

When the drive is stopped, disable all folder rows. When mounted, open each
folder only if its path exists; show a disabled, specific row for a missing
optional folder rather than opening an invalid path. The crash-log directory
must be created by the device package, so it should normally exist even before
the first crash.

If the user deletes or renames the mount while Finder is open, report the
failure and leave controller state governed by the local mount check.

## Required `pistomp-recovery` work

1. Add a persistent crash-report writer that accepts `CrashInfo` during the
   existing recovery startup and captures systemd metadata plus a bounded,
   invocation-focused journal.
2. Add the 50 MiB locking/atomic-write/creation-time rotation implementation.
3. Add tests for first report, repeated recovery handoffs, malformed
   filenames, oversized report truncation, exact budget boundary, and
   creation-time ordering.
4. Keep `diagnose_crash()` and the LCD crash screen behavior intact while
   persisting the same diagnosis before the UI starts.

## Required Companion work

1. Add `SSHDriveController` and a bounded long-lived process lifecycle.
2. Add `SSH Drive` and `Folders` to `AppDelegate`'s menu construction and
   render them from controller/reachability state.
3. Add Finder opening for the four fixed remote paths.
4. Add dependency detection and an actionable missing-macFUSE/SSHFS message.
5. Integrate drive shutdown into `applicationShouldTerminate` without
   regressing the existing JackBridge shutdown barrier.
6. Add user-facing documentation for installing the supported SSHFS stack,
   approving macFUSE system software, and the unprivileged root mount model.
7. Add diagnostic logging for detected SSHFS path/version, mount command exit,
   unmount result, and unexpected process termination. Never log private key
   contents or command-line secrets.

## Verification plan

### Crash writer

1. Trigger a managed service failure that enters crash recovery and verify one
   report appears in `/home/pistomp/.pistomp-recovery/crash-logs/`.
2. Verify the report names the primary service and includes all diagnosed
   service results, invocation metadata, timestamped journal lines, and an
   explicit empty-log indication when journal access fails.
3. Trigger several service failures that converge on one recovery handoff and
   verify they produce one handoff report, not duplicate reports from a new
   capture hook.
4. Simulate concurrent writer invocations and verify no partial files,
   filename collisions, or lost complete reports.
5. Fill the directory beyond 50 MiB and verify rotation leaves total report
   bytes at or below exactly 50 MiB while retaining the newest report and
   ordering strictly by filesystem creation time.
6. Change report mtimes without changing their creation times and verify
   rotation order is unchanged.
7. Create a single oversized diagnostic and verify it is explicitly truncated
   and the budget remains enforced.
8. Reinstall/upgrade `pistomp-recovery` and verify existing reports remain
   readable and are not reset.
9. Verify the LCD still enters crash recovery and still shows its immediate
   journal/log view when report persistence is unavailable.

### SSH Drive

1. With the pi unreachable, verify `SSH Drive` is disabled.
2. With the pi reachable and SSHFS installed, select `SSH Drive` and verify a
   mounted user-owned volume appears without a password prompt or local sudo.
3. Verify the mounted root shows remote `/`, but access remains constrained to
   what `pistomp` can read; verify root-only paths are not elevated by the
   mount.
4. Verify the checkmark and Folders rows become active only after the mount
   passes its local mounted-filesystem check.
5. Open Home, pi-Stomp, pedalboards, and crash logs from Finder and verify
   each reaches the expected remote path.
6. Stop the drive and verify the process exits, the mount is unmounted, the
   checkmark clears, and folder rows disable.
7. Drop the network while mounted and verify the app remains responsive,
   reports the failure, and still permits explicit stop/unmount.
8. Quit the app while mounted and verify it performs bounded cleanup without
   leaving an app-owned mount behind.
9. Start with no local SSHFS/macFUSE installation and verify the menu remains
   safe and the error explains the missing prerequisite and official install
   route.
10. Verify a rejected SSH key follows the existing key-install path rather
    than opening a hidden interactive prompt.
11. Verify the menu never starts SSH Drive automatically after app launch,
    reachability, or a crash recovery event.

### Dependency and release checks

- Test the supported official SSHFS/macFUSE combination on the project's
  minimum macOS version and on the current Apple Silicon development system.
- Test the optional FUSE-T path only as an explicit research/build experiment;
  do not distribute it until the commercial-bundling license is resolved.
- Confirm the app package does not silently vendor the macFUSE kernel/system
  component or an unlicensed FUSE-T binary.
- Exercise `open`/Finder behavior against a real pi, not only a fake local
  directory. A local mock can cover controller state transitions, but it does
  not prove SFTP permissions or remote path mapping.

## Rollout order

1. Implement and test the persistent crash writer in `pistomp-recovery`.
2. Package it through `pi-gen-pistomp` and validate creation-time support on a
   test pi image.
3. Deploy to a test pi and verify reports across single failures and recovery
   handoffs.
4. Add the Companion SSHFS process controller with a fake-process seam.
5. Validate the supported macFUSE/SSHFS dependency on the minimum macOS.
6. Add menu state, checked toggle, Folders submenu, and Finder path opening.
7. Exercise network loss, app termination, stale mount cleanup, and key errors.
8. Document installation, permissions, report location, and rotation before
   release.

## Non-goals

- Granting the Mac or `pistomp` root access through SSHFS.
- Mounting with `allow_other` or changing device sudo policy for Finder.
- Automatically uploading crash reports to TreeFallSound.
- Capturing arbitrary system services or copying coredumps to the SD card.
- Auto-starting, auto-remounting, or login-persisting SSH Drive.
- Bundling FUSE-T without a license decision or bundling macFUSE system
  components inside the Companion application.

## Research sources

- [macFUSE SSHFS wiki](https://github.com/macfuse/macfuse/wiki/File-Systems-%E2%80%90-SSHFS)
- [Upstream libfuse SSHFS](https://github.com/libfuse/sshfs)
- [macFUSE project](https://github.com/macfuse/macfuse)
- [macFUSE website](https://macfuse.github.io/)
- [FUSE-T project](https://github.com/macos-fuse-t/fuse-t)
- [FUSE-T license](https://raw.githubusercontent.com/macos-fuse-t/fuse-t/master/License.txt)
- [gromgit macOS FUSE tap](https://github.com/gromgit/homebrew-fuse)
