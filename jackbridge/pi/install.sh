#!/usr/bin/env bash
# Idempotent installer for the pi-stomp-jackbridge service, for developing
# against a live device by hand.
#
# NOT the image path. Images install the jackbridge .deb, which picks its own
# helpers out of bin/ (see pi-gen-pistomp debpkgs/jackbridge/debian/rules) --
# that list is the image's call, not ours, so it may be a subset of what this
# script installs. Deploy a matched set or none: bin/jackbridge-pi-up and
# bin/jackbridge-xrun-watcher agree on an argv contract, and copying one
# without the other leaves the unit crash-looping on a usage error.
#
# Restarts nothing -- the LCD UI owns enable/disable, and we deliberately do
# not `systemctl enable` here.
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
install -m 0755 "$SRC_DIR/bin/jackbridge-napi-rt"      "$LIBEXEC/"
install -m 0755 "$SRC_DIR/bin/jackbridge-napi-rt-down" "$LIBEXEC/"
install -m 0755 "$SRC_DIR/bin/jackbridge-pi-status"    "$LIBEXEC/"
install -m 0644 "$SRC_DIR/pi-stomp-jackbridge.service" "$UNIT_DIR/"

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
fi

echo "pi-stomp-jackbridge installed under $LIBEXEC + $UNIT_DIR."
echo "jackbridge-pi-status for details."
