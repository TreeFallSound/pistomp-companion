#!/usr/bin/env bash
# Kick the JackBridge LaunchAgents in the active GUI session. Use after
# editing /Library/Application Support/JackBridge/config.plist.
#
# JitterFrames-only changes need just the daemon. Rate/period/ClockDeviceUID
# changes also need coreaudiod (so the HAL re-publishes device properties)
# — pass --hal to include that.
set -euo pipefail

UID_AQUA=$(id -u)
DOMAIN="gui/${UID_AQUA}"

bounce_hal=0
for arg in "$@"; do
    case "$arg" in
        --hal) bounce_hal=1 ;;
        -h|--help)
            echo "usage: $0 [--hal]"
            echo "  (no flags) restart daemon + jackd"
            echo "  --hal     also killall coreaudiod (needed for rate/period changes)"
            exit 0
            ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

echo "== kickstart com.jackbridge.daemon (${DOMAIN}) =="
launchctl kickstart -k "${DOMAIN}/com.jackbridge.daemon"

echo "== kickstart com.jackbridge.jackd (${DOMAIN}) =="
launchctl kickstart -k "${DOMAIN}/com.jackbridge.jackd"

if [[ "$bounce_hal" -eq 1 ]]; then
    echo "== killall coreaudiod (HAL reload) =="
    sudo killall coreaudiod
fi

echo
echo "give it ~2s, then tail with:"
echo "  log stream --predicate 'subsystem == \"com.jackbridge\"' --info"
