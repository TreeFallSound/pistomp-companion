#!/usr/bin/env bash
# Idempotent installer for the pi-stomp-jackbridge service.
#
# Intended to be invoked during the pistomp-arch image build, but safe to run
# by hand on a live device (just doesn't restart anything — the LCD UI owns
# enable/disable, and we explicitly don't `systemctl enable` here).
set -euo pipefail

PREFIX=${PREFIX:-/usr/local}
LIBEXEC="$PREFIX/libexec/jackbridge"
UNIT_DIR=${UNIT_DIR:-/usr/lib/systemd/system}
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

install -d "$LIBEXEC"
install -m 0755 "$SRC_DIR/bin/jackbridge-pi-up"        "$LIBEXEC/"
install -m 0755 "$SRC_DIR/bin/jackbridge-pi-down"      "$LIBEXEC/"
install -m 0755 "$SRC_DIR/bin/jackbridge-xrun-watcher" "$LIBEXEC/"
install -m 0755 "$SRC_DIR/bin/jb-detect-net-iface"     "$LIBEXEC/"
install -m 0755 "$SRC_DIR/bin/jackbridge-pin-route"    "$LIBEXEC/"
install -m 0755 "$SRC_DIR/bin/jackbridge-unpin-route"  "$LIBEXEC/"
install -m 0644 "$SRC_DIR/pi-stomp-jackbridge.service" "$UNIT_DIR/"

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
fi

echo "pi-stomp-jackbridge installed under $LIBEXEC + $UNIT_DIR."
echo "The LCD UI enables/disables it on demand — no systemctl enable here."
