#!/bin/sh
# Resolve the Pi hostname from the installed config.plist.
# Falls back to the shipped default for pre-setting installs.
set -eu

CONFIG=${JACKBRIDGE_CONFIG:-/Library/Application Support/JackBridge/config.plist}
if [ ! -f "$CONFIG" ]; then
    CONFIG=$(CDPATH= cd -- "$(dirname -- "$0")/../installer" && pwd)/config.plist
fi

hostname=$(/usr/libexec/PlistBuddy -c 'Print :PiHostname' "$CONFIG" 2>/dev/null || true)
case "$hostname" in
    ''|*[!A-Za-z0-9._:%-]*) printf '%s\n' 'pistomp.local' ;;
    *) printf '%s\n' "$hostname" ;;
esac
