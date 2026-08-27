#!/usr/bin/env bash
# reinstall.sh — one-shot: install the freshly built .pkg, wipe the shm
# region (so a protocol-version bump doesn't leave both sides refusing to
# attach), restart coreaudiod (forces the HAL driver to re-mmap), and
# bootcycle the two LaunchAgents in the right order (jackd first so the
# daemon doesn't race a not-yet-running server).
#
# Assumes ./jackbridge/installer/build-pkg.sh has already produced
# ./jackbridge/installer/build/PiStompCompanion-<version>.pkg. Picks the newest
# package in that directory if multiple exist.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PKG=$(ls -t "$ROOT/jackbridge/installer/build"/PiStompCompanion-*.pkg 2>/dev/null | head -1 || true)
if [ -z "$PKG" ]; then
    echo "no .pkg found under jackbridge/installer/build/ — run ./jackbridge/installer/build-pkg.sh first" >&2
    exit 1
fi

SUPPORT="/Library/Application Support/JackBridge"

echo "==> installing $(basename "$PKG")"
sudo installer -pkg "$PKG" -target /

echo "==> installer leaves GUI LaunchAgents stopped"

echo "==> bouncing coreaudiod (releases HAL's shm mapping)"
sudo killall coreaudiod 2>/dev/null || true

echo "==> unlinking shm regions"
# Prefer the installed binary if present (next pkg bump ships it); fall back
# to a libc ctypes one-liner so this still works on the current install.
if [ -x "$SUPPORT/jb-rmshm" ]; then
    "$SUPPORT/jb-rmshm"
else
    python3 - <<'PY'
import ctypes
libc = ctypes.CDLL("libc.dylib")
for name in (b"/JackBridge", b"/jackrouter", b"/jackrouter2"):
    libc.shm_unlink(name)
PY
fi

echo "==> leaving LaunchAgents stopped; launch the Companion to start JACK"

echo "==> done"
