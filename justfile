# PiStomp Companion dev loops. `just --list` to see everything.
#
# ── How this file is organised ────────────────────────────────────────────────
#
# Recipes come in four layers, and each layer does only its own job:
#
#   build    compiles into the build tree. Touches nothing outside it.
#   install  copies build output into system paths. Needs sudo. Never restarts.
#   control  restarts, drops the shm region, reports health. Never installs.
#   loop     what you actually type. Composes the three layers above.
#
# One rule keeps them composable:
#
#   install never restarts, control never installs.
#
# That is what lets you chain any number of installs and then restart exactly
# once. Two recipes that each restart cannot be chained — see the ordering
# constraint below for why the second restart is actively harmful.
#
# ── The one ordering constraint ───────────────────────────────────────────────
#
#   install  →  unlink-shm  →  restart
#
# Always in that order. The HAL driver creates the /JackBridge shm region in
# _HW_Open, and _HW_Open runs only when coreaudiod loads the plug-in. So the
# unlink has to land after the new binaries are in place and before the
# restart that loads them. Unlink *after* the restart and the name stays gone:
# nothing recreates it, and JackBridged crash-loops on ENOENT until the next
# coreaudiod bounce.
#
# ── Two loops ─────────────────────────────────────────────────────────────────
#
#   inner  `just reload`        edit C++ or a helper → swap → bounce
#   outer  `just pkg-install`   the authoritative install path, sudo
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

# Default: rebuild the engine and bounce it live.
default: reload

# ── build ─────────────────────────────────────────────────────────────────────
# Compile only. No sudo, no system paths, no services touched.

# Build the HAL driver target only.
driver:
    xcodebuild -project {{proj}} -target JackBridgePlugIn -configuration Release \
        ARCHS={{archs}} ONLY_ACTIVE_ARCH=NO "JACK_PREFIX={{jack_prefix}}" | tail -3

# Build the daemon target only.
daemon:
    xcodebuild -project {{proj}} -target JackBridged -configuration Release \
        ARCHS={{archs}} ONLY_ACTIVE_ARCH=NO "JACK_PREFIX={{jack_prefix}}" | tail -3

# Build both engine targets plus jb-rmshm.
engine: driver daemon
    clang -O2 -o "{{engine_out}}/jb-rmshm" jackbridge/tools/rmshm.c
    @echo "engine built → {{engine_out}}"

# Build the menu-bar app (no install; it reads shm live).
app:
    xcodebuild -project app/PiStompCompanion.xcodeproj -target PiStompCompanion \
        -configuration Release ARCHS={{archs}} ONLY_ACTIVE_ARCH=NO | tail -3
    @echo "app built → {{app_out}}/PiStompCompanion.app"

# ── install ───────────────────────────────────────────────────────────────────
# Copy into system paths. sudo. None of these restart anything — that is what
# makes them safe to chain. Follow with `just restart` (or use a loop recipe).

# Copy the built driver, daemon and jb-rmshm into place. No restart.
install-engine: engine
    sudo cp -R "{{engine_out}}/JackBridgePlugIn.driver" "{{sys_hal}}/"
    sudo cp "{{engine_out}}/JackBridged" "{{engine_out}}/jb-rmshm" "{{sys_support}}/"

# These ship only in the .pkg, so a one-line change to any of them otherwise
# costs a full `just pkg-install`: four xcodebuild runs (one a `clean build`)
# plus pkgbuild and productbuild, to deliver a text file.
#
# `install -m 0755`, not `cp`: it stamps the mode explicitly. `cp` onto an
# existing file inherits that file's mode and is fine, but a helper not yet at
# the destination would take root's umask and can land non-executable — which
# launchd reports as a service that will not stay up, not as a permissions
# problem.
#
# NOT covered, because the .pkg generates or templates them and a raw copy
# would write the wrong thing: jack-prefix and config.plist (stamped with
# JACK_PREFIX at package time) and the LaunchAgent / LaunchDaemon plists
# (which carry a per-user path). Change one of those and you need
# `just pkg-install`.
#
# Copy the shell helpers into place. No build, no restart.
install-scripts:
    #!/usr/bin/env bash
    set -euo pipefail
    helpers=(
        jackd-launch jb-detect-net-iface jb-is-wifi-iface
        jackbridge-pin-route jackbridge-route-watcher
        jackbridge-jackd jackbridge-coordinator jack-prefix.sh
    )
    # Syntax-check before overwriting anything. These run under launchd, where
    # a syntax error surfaces as a service that will not stay up rather than
    # as a parse error you can read.
    for f in "${helpers[@]}"; do sh -n "jackbridge/installer/$f"; done
    sh -n jackbridge/tools/jackbridge-ctl
    for f in "${helpers[@]}"; do
        sudo install -m 0755 "jackbridge/installer/$f" "{{sys_support}}/$f"
    done
    sudo install -m 0755 jackbridge/tools/jackbridge-ctl "{{sys_support}}/jackbridge-ctl"
    echo "==> installed ${#helpers[@]} helpers + jackbridge-ctl"

# Install everything the .pkg would, except what it templates. No restart.
install: install-engine install-scripts

# ── control ───────────────────────────────────────────────────────────────────
# Act on the running stack. These never build and never install.

# Bounce coreaudiod, the plug-in host, and the two agents. No rebuild.
restart:
    #!/usr/bin/env bash
    set -euo pipefail
    # coreaudiod first, because the plug-in creates the shm region in _HW_Open
    # and _HW_Open runs only when the plug-in is loaded. coreaudiod respawns
    # lazily and does not load HAL plug-ins until something enumerates
    # devices — so `killall` alone leaves the region absent, and the
    # system_profiler call below is what actually forces it back into
    # existence. The plug-in's host process is killed too: it is a separate
    # helper that outlives a coreaudiod bounce, and a survivor holds a mapping
    # of the old region while never rebuilding the name.
    sudo killall coreaudiod || true
    sudo killall "Core Audio Driver (JackBridgePlugIn.driver)" || true
    sleep 2
    system_profiler SPAudioDataType >/dev/null 2>&1 || true
    # Then jackd before the daemon: JackBridged exits 1 on a failed
    # jack_client_open, and launchd answers that with ThrottleInterval, not
    # with a useful error.
    # kickstart -k only restarts a service that is already bootstrapped, and
    # the app owns the stack: quitting it runs `jackbridge-ctl stop`, which
    # boots both agents out of the domain. Running `just reload` with the app
    # closed then failed with "Could not find service ... in domain for user
    # gui: 501" and brought nothing up. Bootstrap first when absent — same
    # rule as jackbridge-ctl's bootstrap_agent.
    for label in com.treefallsound.companion.jackd com.treefallsound.companion.daemon; do
        if launchctl print "{{gui}}/$label" >/dev/null 2>&1; then
            launchctl kickstart -k "{{gui}}/$label" || true
        else
            launchctl enable "{{gui}}/$label" 2>/dev/null || true
            launchctl bootstrap "{{gui}}" "/Library/LaunchAgents/$label.plist" || true
        fi
    done

# Unlink the /JackBridge shm region. No restart — see the ordering constraint.
unlink-shm:
    #!/usr/bin/env bash
    set -euo pipefail
    # Prefer the installed helper. Before the first pkg install there isn't
    # one, so build the local copy — into the build tree, not /tmp: this runs
    # under sudo, and a root-run binary in a world-writable directory is a
    # handout. jb-rmshm always exits 0 (it ignores "no such shm"), so presence
    # is the only condition worth testing.
    if [ -x "{{sys_support}}/jb-rmshm" ]; then
        sudo "{{sys_support}}/jb-rmshm"
    else
        mkdir -p "{{engine_out}}"
        clang -O2 -o "{{engine_out}}/jb-rmshm" jackbridge/tools/rmshm.c
        sudo "{{engine_out}}/jb-rmshm"
    fi

# Show the CoreAudio device entry. Should read "pi-Stomp (<host>)".
device-name:
    @system_profiler SPAudioDataType 2>/dev/null | grep -A6 -i "stomp\|jackbridge" || \
     echo "not found — is the stack up? (just status)"

# Tail engine logs (ctrl-C to stop).
logs:
    log stream --predicate 'subsystem == "com.treefallsound.companion"' --info

# One-line stack health from the installed ctl tool.
status:
    "{{sys_support}}/jackbridge-ctl" status || echo "jackbridge-ctl not installed — run just pkg-install first"

# ── loops ─────────────────────────────────────────────────────────────────────
# Compositions of the layers above. Each restarts exactly once, at the end.

# Build, install, drop the shm, bounce. The engine inner loop.
reload: install unlink-shm
    just restart
    @echo "stack reloaded. give it ~3s, then: just device-name"

# Reload the engine and the menu-bar app together.
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
    just app-restart "{{config}}"

# Install the shell helpers and bounce. Skips the Xcode builds entirely.
reload-scripts: install-scripts
    just restart


# Rebuild the current ../jack2 fork and install it directly into /usr/local.
# The helper warns before building a dirty fork tree, so uncommitted changes
# are included deliberately.
#
# Build + install only, per the layering rule. The helper boots the agents out
# first (libjackserver.so is "text file busy" otherwise), so the stack is down
# when this returns — bring it back with `just restart` or by launching the
# Companion.
jack-rebuild:
    ./jack-rebuild-mac.sh
    @echo "JACK rebuilt and installed. bring the stack back with: just restart"

# Which fork build is installed? (never judge this by timestamps)
jack-verify:
    #!/usr/bin/env bash
    set -uo pipefail
    # netJACK2 lives in the loadable internal clients. jackd itself carries
    # none of it, so its mtime is not evidence of anything.
    printf '%-46s %-22s %s\n' ARTIFACT MTIME MARKERS
    check() {
        f="$1"; shift
        [ -f "$f" ] || { printf '%-46s %-22s %s\n' "$f" "ABSENT" "-"; return; }
        m=$(stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$f")
        out=""
        for s in "$@"; do
            if strings -a "$f" | grep -qF "$s"; then out="$out ok:$s"; else out="$out MISSING:$s"; fi
        done
        printf '%-46s %-22s %s\n' "$f" "$m" "$out"
    }
    check /usr/local/lib/jack/netmanager.so "pinning masters to ifindex" "JACK_NETJACK_MULTICAST_IF"
    check /usr/local/lib/libjackserver.0.dylib "SetMulticastIF: if_nametoindex"
    echo
    echo "running jackd (loads netmanager.so at jack_load time):"
    pgrep -lf 'bin/jackd' || echo "  not running"

# Drop a stale shm region and bounce. Recovery after a protocol bump.
rmshm: unlink-shm
    just restart

# ── package (outer loop) ──────────────────────────────────────────────────────

# Build a real installer package.
pkg:
    ./jackbridge/installer/build-pkg.sh {{version}}

# Build and install the package (authoritative path).
pkg-install: pkg
    sudo installer -pkg "{{pkg}}" -target /
    @echo "installed. stack restarts via postinstall; verify with: just device-name"

# ── app ───────────────────────────────────────────────────────────────────────

# App inner loop: kill the running copy, rebuild, relaunch detached.
app-restart config="Debug":
    ./app-restart.sh "{{config}}"

# Run the app from its build tree (shares the live shm, no install).
run-app: app
    open "{{app_out}}/PiStompCompanion.app"

# The app owns the stack: launching it starts jackd + the daemon, quitting
# tears them down.
#
# Launch the installed menu-bar app (no rebuild).
open-app:
    open -a PiStompCompanion

# ── misc ──────────────────────────────────────────────────────────────────────

# Remove both build trees.
clean:
    rm -rf jackbridge/driver/build app/build jackbridge/installer/build
