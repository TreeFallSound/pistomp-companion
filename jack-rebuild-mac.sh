#!/usr/bin/env bash
# Rebuild jack2 from /Users/cam/dev/jack2 (the sastraxi/jack2 fork) and
# install it over the /usr/local jackd. Then deploy the current
# jackd-launch (which sources JACK_NETJACK_MULTICAST_IF from the route
# daemon's state file) and re-bootstrap the LaunchAgents.
#
# Run as the operator user; the script will sudo for the install + agent
# bootstrap.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
JACK_SRC="${JACK_SRC:-../jack2}"
SUPPORT="/Library/Application Support/JackBridge"
LA_UID="$(id -u)"

if [ "$(id -u)" -eq 0 ]; then
    echo "do not run as root; the script will sudo just the install + cp steps" >&2
    exit 1
fi

echo "==> preflight: fork HEAD should be on the multicast-pin work"
cd "$JACK_SRC"
git log --oneline -3
git status --short
# Bail if the working tree is dirty — otherwise the rebuild silently
# bakes in a half-finished edit.
if [ -n "$(git status --porcelain)" ]; then
    echo "error: jack2 working tree is dirty; commit or stash first" >&2
    exit 1
fi

echo
echo "==> bootout LaunchAgents (uid=$LA_UID) and kill stragglers"
launchctl bootout "gui/$LA_UID/com.treefallsound.companion.daemon" 2>/dev/null || true
launchctl bootout "gui/$LA_UID/com.treefallsound.companion.jackd"  2>/dev/null || true
# bootout SIGTERMs the wrapper, not the background jackd child. Kill
# jackd explicitly so the next install step doesn't fail on
# "text file busy" for libjackserver.so.
sudo pkill -9 jackd       2>/dev/null || true
sudo pkill -9 JackBridged 2>/dev/null || true
sleep 1

echo
echo "==> configure + build"
# /usr/local is where the existing jackd lives on this Apple Silicon
# Mac. On Apple Silicon, /usr/local is the manual-install prefix
# (Homebrew is at /opt/homebrew and the two are intentionally separate).
# --prefix=/usr/local is right for overwriting the manual install.
#
# CPPFLAGS+LDFLAGS point waf at /opt/homebrew because v1.9.22's wscript
# has a hard `conf.check(lib='aften')` on macOS (the AC-3 audioadapter
# encoder), and coreaudio/JackAC3Encoder.h includes <aften/aften.h>
# unconditionally. aften ships in Homebrew (brew install aften). We
# don't *use* aften in our build — the netJACK2 path doesn't encode
# AC-3 — but the configure check and the coreaudio compile both
# require it. Pointing the build at Homebrew's aften is enough; the
# AC-3 encoder is only loaded if a client asks for it.
CPPFLAGS="-I/opt/homebrew/include" \
LDFLAGS="-L/opt/homebrew/lib" \
    python3 ./waf configure --prefix=/usr/local
CPPFLAGS="-I/opt/homebrew/include" \
LDFLAGS="-L/opt/homebrew/lib" \
    python3 ./waf build

echo
echo "==> install (sudo)"
sudo python3 ./waf install

echo
echo "==> deploy the current jackd-launch (sets JACK_NETJACK_MULTICAST_IF)"
sudo cp "$REPO_ROOT/jackbridge/installer/jackd-launch" \
        "$SUPPORT/jackd-launch"
sudo chmod +x "$SUPPORT/jackd-launch"

echo
echo "==> leave LaunchAgents disabled; launch the Companion to start JACK"

echo
echo "==> verify"
pgrep -lf 'jackd|JackBridged' || true
/usr/local/bin/jackd --version
echo
echo "JACK_NETJACK_MULTICAST_IF landed in netmanager.so:"
strings /usr/local/lib/jack/netmanager.so | grep -F JACK_NETJACK_MULTICAST_IF || echo "  NOT FOUND — rebuild did not pick up the fork's commits"
