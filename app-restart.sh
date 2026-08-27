#!/usr/bin/env bash
# app-restart.sh — dev loop for the menu-bar app: kill any running copy,
# rebuild it, relaunch it detached.
#
# The relaunch goes through `open`, so the app is parented to launchd rather
# than to this shell — it survives the terminal closing, and nothing here
# waits on it. Build output is quiet unless xcodebuild fails, in which case
# the full log is dumped and the old app stays dead (loud, not silent).
#
#   ./app-restart.sh              # Debug
#   ./app-restart.sh Release      # any xcodebuild configuration
#   ./app-restart.sh --no-launch  # kill + build only

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
CONFIG=Debug
LAUNCH=1
for arg in "$@"; do
    case "$arg" in
        --no-launch) LAUNCH=0 ;;
        -*) echo "unknown flag: $arg" >&2; exit 2 ;;
        *) CONFIG="$arg" ;;
    esac
done

APP="$ROOT/build/Build/Products/$CONFIG/PiStompCompanion.app"
LOG="$(mktemp -t pistomp-companion-build)"
trap 'rm -f "$LOG"' EXIT

# --- kill -------------------------------------------------------------
# Match on the executable name, not the bundle path: a copy launched from
# /Applications or from a stale derived-data dir would otherwise survive and
# you'd end up staring at two menu-bar icons.
if pgrep -x PiStompCompanion >/dev/null; then
    echo "==> stopping running PiStompCompanion"
    pkill -x PiStompCompanion || true
    for _ in $(seq 20); do
        pgrep -x PiStompCompanion >/dev/null || break
        sleep 0.1
    done
    if pgrep -x PiStompCompanion >/dev/null; then
        echo "    still up after 2s — SIGKILL"
        pkill -9 -x PiStompCompanion || true
        sleep 0.3
    fi
fi

# --- build ------------------------------------------------------------
echo "==> building ($CONFIG)"
if ! xcodebuild \
        -project "$ROOT/app/PiStompCompanion.xcodeproj" \
        -scheme PiStompCompanion \
        -configuration "$CONFIG" \
        -derivedDataPath "$ROOT/build" \
        build >"$LOG" 2>&1; then
    echo "==> BUILD FAILED" >&2
    cat "$LOG" >&2
    exit 1
fi

if [ ! -d "$APP" ]; then
    echo "==> build reported success but $APP is missing" >&2
    exit 1
fi

# --- launch -----------------------------------------------------------
if [ "$LAUNCH" -eq 0 ]; then
    echo "==> built $APP (not launching)"
    exit 0
fi

echo "==> launching $APP"
open -a "$APP"

for _ in $(seq 30); do
    if pgrep -x PiStompCompanion >/dev/null; then
        echo "==> running (pid $(pgrep -x PiStompCompanion | tr '\n' ' '))"
        exit 0
    fi
    sleep 0.1
done

echo "==> launched, but no PiStompCompanion process appeared within 3s" >&2
exit 1
