# PiStomp Companion dev loops. `just --list` to see everything.
#
# Two loops:
#   inner — `just reload`   (edit C++ → rebuild → swap binaries → bounce stack)
#   outer — `just pkg-install` (authoritative .pkg install path, sudo)
#
# The engine installs to system paths and is a coreaudiod plug-in, so any
# recipe touching /Library needs sudo. The app is a normal binary — run it
# from the build tree, no install.

version        := "0.3.0"
jack_prefix    := env_var_or_default("JACK_PREFIX", "/usr/local")
archs          := "arm64"
proj           := "jackbridge/driver/JackBridgePlugIn.xcodeproj"
engine_out     := "jackbridge/driver/build/Release"
app_out        := "app/build/Release"
sys_support    := "/Library/Application Support/JackBridge"
sys_hal        := "/Library/Audio/Plug-Ins/HAL"
gui            := "gui/" + `id -u`
pkg            := "jackbridge/installer/build/PiStompCompanion-" + version + ".pkg"

# ── engine targets ────────────────────────────────────────────────────────────

# Default: rebuild the engine and bounce it live.
default: reload

# Build the HAL driver target only.
driver:
    xcodebuild -project {{proj}} -target JackBridgePlugIn -configuration Release \
        ARCHS={{archs}} ONLY_ACTIVE_ARCH=NO "JACK_PREFIX={{jack_prefix}}" | tail -3

# Build the daemon target only.
daemon:
    xcodebuild -project {{proj}} -target JackBridged -configuration Release \
        ARCHS={{archs}} ONLY_ACTIVE_ARCH=NO "JACK_PREFIX={{jack_prefix}}" | tail -3

# Build both engine targets plus jb-rmshm (postinstall depends on the
# installed copy).
engine: driver daemon
    clang -O2 -o "{{engine_out}}/jb-rmshm" jackbridge/tools/rmshm.c
    @echo "engine built → {{engine_out}}"

# Build the menu-bar app (no install; it reads shm live).
app:
    xcodebuild -project app/PiStompCompanion.xcodeproj -target PiStompCompanion \
        -configuration Release ARCHS={{archs}} ONLY_ACTIVE_ARCH=NO | tail -3
    @echo "app built → {{app_out}}/PiStompCompanion.app"

# ── live reload (inner loop) ──────────────────────────────────────────────────

# Install freshly-built driver+daemon over the system copies, restart everything.
# This is the engine inner loop: edit C++ → `just reload` → check.
reload: engine
    sudo cp -R "{{engine_out}}/JackBridgePlugIn.driver" "{{sys_hal}}/"
    sudo cp "{{engine_out}}/JackBridged" "{{engine_out}}/jb-rmshm" "{{sys_support}}/"
    sudo killall coreaudiod || true
    launchctl kickstart -k "{{gui}}/com.treefallsound.companion.daemon" || true
    launchctl kickstart -k "{{gui}}/com.treefallsound.companion.jackd" || true
    @echo "stack reloaded. give it ~3s, then: just device-name"

# Force-reload the whole stack including the menu-bar app. Use after a
# protocol bump, when `just reload` alone leaves a stale shm region behind.
# Does NOT refresh jackbridge-ctl or the LaunchAgent plists — those ship only
# in the .pkg, so a change to either still needs `just pkg-install`.
reload-all config="Release":
    #!/usr/bin/env bash
    set -euo pipefail
    # Quit through AppleScript, not pkill: applicationShouldTerminate shells
    # out to `jackbridge-ctl stop` and defers termination until it returns,
    # and SIGTERM does not reliably reach that path. The app is also what
    # brings the stack back up (`jackbridge-ctl start` on launch), so it goes
    # down first and comes up last.
    if pgrep -x PiStompCompanion >/dev/null; then
        echo "==> quitting PiStompCompanion"
        osascript -e 'quit app "PiStompCompanion"' || true
        for _ in $(seq 30); do
            pgrep -x PiStompCompanion >/dev/null || break
            sleep 0.2
        done
        pkill -9 -x PiStompCompanion 2>/dev/null || true
    fi
    just reload
    # Order matters: new binaries first, then unlink — see the rmshm recipe.
    just rmshm
    just app-restart "{{config}}"

# Bounce the two agents + coreaudiod without rebuilding (config change).
restart:
    launchctl kickstart -k "{{gui}}/com.treefallsound.companion.daemon" || true
    launchctl kickstart -k "{{gui}}/com.treefallsound.companion.jackd" || true
    sudo killall coreaudiod || true

# Drop the stale shm region (needed after a protocol bump) and restart.
rmshm:
    #!/usr/bin/env bash
    set -euo pipefail
    # Order matters: install the new binaries FIRST (`just reload`), then run
    # this. Dropping the region while the old binaries are still running only
    # lets them recreate it and stamp the old version back in — and
    # check_protocol_version() writes a version only into a region reading 0,
    # so a stale stamp survives until the region is unlinked again.
    # Prefer the installed helper. Before the first pkg install there isn't
    # one, so build the local copy — into the build tree, not /tmp: this runs
    # under sudo, and a root-run binary in a world-writable directory is a
    # handout. rmshm always exits 0 (it ignores "no such shm"), so presence is
    # the only condition worth testing.
    if [ -x "{{sys_support}}/jb-rmshm" ]; then
        sudo "{{sys_support}}/jb-rmshm"
    else
        mkdir -p "{{engine_out}}"
        clang -O2 -o "{{engine_out}}/jb-rmshm" jackbridge/tools/rmshm.c
        sudo "{{engine_out}}/jb-rmshm"
    fi
    just restart

# ── outer loop (.pkg) ─────────────────────────────────────────────────────────

# Build a real installer package.
pkg:
    ./jackbridge/installer/build-pkg.sh {{version}}

# Build and install the package (authoritative path).
pkg-install: pkg
    sudo installer -pkg "{{pkg}}" -target /
    @echo "installed. stack restarts via postinstall; verify with: just device-name"

# ── observe ───────────────────────────────────────────────────────────────────

# Show the CoreAudio device entry (name, channels, manufacturer). After the
# rename this should read "pi-Stomp (<host>)".
device-name:
    @system_profiler SPAudioDataType 2>/dev/null | grep -A6 -i "stomp\|jackbridge" || \
     echo "not found — is the stack up? (just status)"

# Tail engine logs (ctrl-C to stop).
logs:
    log stream --predicate 'subsystem == "com.treefallsound.companion"' --info

# One-line stack health from the installed ctl tool.
status:
    "{{sys_support}}/jackbridge-ctl" status || echo "jackbridge-ctl not installed — run just pkg-install first"

# ── misc ──────────────────────────────────────────────────────────────────────

# Launch the installed menu-bar app (no rebuild). The app owns the stack:
# launching it bootstraps/starts jackd + the daemon, quitting tears them down.
open-app:
    open -a PiStompCompanion

# Run the app from its build tree (shares the live shm, no install).
run-app: app
    open "{{app_out}}/PiStompCompanion.app"

# App inner loop: kill the running copy, rebuild, relaunch detached.
# Wrapper for ./app-restart.sh. Pass a configuration to build non-Debug.
app-restart config="Debug":
    ./app-restart.sh "{{config}}"

# Remove both build trees.
clean:
    rm -rf jackbridge/driver/build app/build jackbridge/installer/build
