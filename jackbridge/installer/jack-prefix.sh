#!/bin/sh
# Resolve the JACK2 prefix stamped into installed runtime data.
jb_resolve_jack_prefix() {
    if [ -n "${JACKBRIDGE_JACK_PREFIX:-}" ]; then
        printf '%s\n' "$JACKBRIDGE_JACK_PREFIX"
        return 0
    fi
    _file="${JACKBRIDGE_PREFIX_FILE:-/Library/Application Support/JackBridge/jack-prefix}"
    if [ -r "$_file" ]; then
        _p=$(sed -n '1p' "$_file")
        case "$_p" in /*) printf '%s\n' "$_p"; return 0 ;; esac
    fi
    for _p in /usr/local /opt/homebrew; do
        if [ -x "$_p/bin/jackd" ]; then printf '%s\n' "$_p"; return 0; fi
    done
    printf '%s\n' /usr/local
}
JACK_PREFIX=$(jb_resolve_jack_prefix)
JACKD="$JACK_PREFIX/bin/jackd"
JACK_LOAD="$JACK_PREFIX/bin/jack_load"
JACK_LSP="$JACK_PREFIX/bin/jack_lsp"
export JACK_PREFIX JACKD JACK_LOAD JACK_LSP
case "$0" in *jack-prefix.sh) printf '%s\n' "$JACK_PREFIX" ;; esac
