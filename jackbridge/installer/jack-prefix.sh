#!/bin/sh
# jack-prefix.sh — resolve the JACK2 install prefix, once, for every runtime
# consumer on the Mac side.
#
# The package is built against exactly one prefix (JACK_PREFIX in
# build-pkg.sh, default /usr/local) and JackBridged dlopens libjack from it.
# A jack_* CLI tool from some *other* prefix talking to that jackd is version
# skew waiting to happen, so nobody gets to guess independently: build-pkg.sh
# stamps the prefix it built against into config.plist as `JackPrefix`, and
# both this script and the Companion's JackTools.swift read it with the same
# precedence.
#
# Order:
#   1. $JACKBRIDGE_JACK_PREFIX          — debugging escape hatch
#   2. JackPrefix in config.plist       — what the package was built against
#   3. first of /usr/local, /opt/homebrew that actually has bin/jackd
#   4. /usr/local                       — documented default
#
# Usage:
#   . "$SUPPORT/jack-prefix.sh"     # sets JACK_PREFIX, JACKD, JACK_LOAD, JACK_LSP
#   ./jack-prefix.sh                # prints the resolved prefix (debugging)

jb_resolve_jack_prefix() {
    _cfg="${1:-/Library/Application Support/JackBridge/config.plist}"

    if [ -n "${JACKBRIDGE_JACK_PREFIX:-}" ]; then
        printf '%s\n' "$JACKBRIDGE_JACK_PREFIX"
        return 0
    fi

    if [ -f "$_cfg" ]; then
        _p=$(/usr/libexec/PlistBuddy -c "Print :JackPrefix" "$_cfg" 2>/dev/null || true)
        # An unstamped template (placeholder, not a path) is ignored rather
        # than exec'd.
        case "$_p" in
            /*) printf '%s\n' "$_p"; return 0 ;;
        esac
    fi

    for _p in /usr/local /opt/homebrew; do
        if [ -x "$_p/bin/jackd" ]; then
            printf '%s\n' "$_p"
            return 0
        fi
    done

    printf '%s\n' /usr/local
}

JACK_PREFIX=$(jb_resolve_jack_prefix "${CONFIG:-}")
JACKD="$JACK_PREFIX/bin/jackd"
JACK_LOAD="$JACK_PREFIX/bin/jack_load"
JACK_LSP="$JACK_PREFIX/bin/jack_lsp"
export JACK_PREFIX JACKD JACK_LOAD JACK_LSP

# Direct invocation: print what we resolved and why nothing else matters.
case "$0" in
    *jack-prefix.sh) printf '%s\n' "$JACK_PREFIX" ;;
esac
