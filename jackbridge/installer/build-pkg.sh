#!/usr/bin/env bash
# Build the PiStomp Companion installer .pkg.
#
# Usage:
#   ./jackbridge/installer/build-pkg.sh [version]
#
# Local dev (ad-hoc): no env vars; produces an unsigned, un-notarized .pkg
# suitable for testing the layout on the build machine only.
#
# Release: set all of
#   SIGN_APP_IDENTITY     "Developer ID Application: <Team> (XXXXXXXXXX)"
#   SIGN_INSTALLER_IDENTITY "Developer ID Installer: <Team> (XXXXXXXXXX)"
#   NOTARY_PROFILE        keychain profile name (xcrun notarytool store-credentials)
# Optional:
#   SKIP_NOTARIZE=1       sign but do not submit (smoke-test the signing chain)

set -euo pipefail

VERSION="${1:-0.1.0}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
JACKBRIDGE="$ROOT/jackbridge"
INSTALLER="$JACKBRIDGE/installer"
BUILD="$INSTALLER/build"
STAGING="$BUILD/staging"
SCRIPTS="$INSTALLER/scripts"

PKG_ID="com.treefallsound.companion"
PKG_OUT="$BUILD/PiStompCompanion-$VERSION.pkg"
JACK_MIN_VERSION="1.9.22"

# Where to find JACK2 headers + dylib at build time. Matches the JACK_PREFIX
# Xcode build setting; override both together when building against a non-
# default prefix (e.g. arm64 Homebrew: JACK_PREFIX=/opt/homebrew).
JACK_PREFIX="${JACK_PREFIX:-/usr/local}"

check_jack() {
    if [ ! -f "$JACK_PREFIX/include/jack/jack.h" ] || [ ! -f "$JACK_PREFIX/lib/libjack.0.dylib" ]; then
        echo "error: JACK2 headers/dylib not found under $JACK_PREFIX." >&2
        echo "       install JACK2 ${JACK_MIN_VERSION}+ from https://github.com/jackaudio/jack2-releases/releases" >&2
        echo "       or override the prefix: JACK_PREFIX=/opt/homebrew $0 ..." >&2
        exit 1
    fi
    if [ -x "$JACK_PREFIX/bin/jackd" ]; then
        ver=$("$JACK_PREFIX/bin/jackd" --version 2>&1 | head -1 | awk '{print $3}')
        if [ -n "$ver" ] && ! printf '%s\n%s\n' "$JACK_MIN_VERSION" "$ver" | sort -V -C; then
            echo "error: JACK2 $ver too old (need ${JACK_MIN_VERSION}+)." >&2
            exit 1
        fi
    fi
}
check_jack

rm -rf "$BUILD"
mkdir -p "$STAGING/Library/Audio/Plug-Ins/HAL"
mkdir -p "$STAGING/Library/Application Support/JackBridge"
mkdir -p "$STAGING/Library/LaunchAgents"
mkdir -p "$STAGING/Library/LaunchDaemons"

XCBUILD_ARGS=(
    -project "$JACKBRIDGE/driver/JackBridgePlugIn.xcodeproj"
    -configuration Release
    CONFIGURATION_BUILD_DIR="$BUILD/xcode"
    "JACK_PREFIX=$JACK_PREFIX"
)
# Default ARCHS is arm64-only because the TreefallSound/jack2 fork's
# build-macos-pkg.sh produces a single-arch libjack. Override with
# ARCHS="arm64 x86_64" for a universal build (requires a universal
# libjack at $JACK_PREFIX).
XCBUILD_ARGS+=(ARCHS="${ARCHS:-arm64}")
if [[ -n "${SIGN_APP_IDENTITY:-}" ]]; then
    XCBUILD_ARGS+=(CODE_SIGN_IDENTITY="$SIGN_APP_IDENTITY" CODE_SIGN_STYLE=Manual OTHER_CODE_SIGN_FLAGS="--timestamp")
fi

echo "==> Building driver + daemon + helpers"
xcodebuild "${XCBUILD_ARGS[@]}" -target JackBridgePlugIn clean build >/dev/null
xcodebuild "${XCBUILD_ARGS[@]}" -target JackBridged build >/dev/null
xcodebuild "${XCBUILD_ARGS[@]}" -target jb-detect-builtin build >/dev/null

echo "==> Building PiStompCompanion menu-bar app"
APP_ARGS=(-project "$ROOT/app/PiStompCompanion.xcodeproj" -target PiStompCompanion
    -configuration Release ARCHS="${ARCHS:-arm64}" ONLY_ACTIVE_ARCH=NO)
if [[ -n "${SIGN_APP_IDENTITY:-}" ]]; then
    APP_ARGS+=(CODE_SIGN_IDENTITY="$SIGN_APP_IDENTITY" CODE_SIGN_STYLE=Manual OTHER_CODE_SIGN_FLAGS="--timestamp")
fi
xcodebuild "${APP_ARGS[@]}" build >/dev/null
APP_BUILD_DIR="$ROOT/app/build/Release"

# rmshm is a 5-line shm_unlink utility — not worth its own Xcode target.
# Shipped so users can recover after a JACKBRIDGE_PROTOCOL_VERSION bump
# without bootcycling agents by hand.
clang -O2 -o "$BUILD/xcode/jb-rmshm" "$JACKBRIDGE/tools/rmshm.c"

cp -R "$BUILD/xcode/JackBridgePlugIn.driver" "$STAGING/Library/Audio/Plug-Ins/HAL/"
cp    "$BUILD/xcode/JackBridged"             "$STAGING/Library/Application Support/JackBridge/"
cp    "$BUILD/xcode/jb-detect-builtin"       "$STAGING/Library/Application Support/JackBridge/"
cp    "$BUILD/xcode/jb-rmshm"                "$STAGING/Library/Application Support/JackBridge/"
install -m 0755 "$INSTALLER/jackd-launch"          "$STAGING/Library/Application Support/JackBridge/jackd-launch"
install -m 0755 "$INSTALLER/jb-detect-net-iface"      "$STAGING/Library/Application Support/JackBridge/jb-detect-net-iface"
install -m 0755 "$INSTALLER/jb-is-wifi-iface"         "$STAGING/Library/Application Support/JackBridge/jb-is-wifi-iface"
install -m 0755 "$INSTALLER/jackbridge-pin-route"     "$STAGING/Library/Application Support/JackBridge/jackbridge-pin-route"
install -m 0755 "$INSTALLER/jackbridge-route-watcher" "$STAGING/Library/Application Support/JackBridge/jackbridge-route-watcher"
install -m 0755 "$JACKBRIDGE/tools/jackbridge-ctl" "$STAGING/Library/Application Support/JackBridge/jackbridge-ctl"
install -m 0755 "$INSTALLER/jackbridge-jackd"       "$STAGING/Library/Application Support/JackBridge/jackbridge-jackd"
install -m 0755 "$INSTALLER/jackbridge-coordinator" "$STAGING/Library/Application Support/JackBridge/jackbridge-coordinator"
printf '%s\n' "$JACK_PREFIX" > "$BUILD/jack-prefix"
install -m 0644 "$BUILD/jack-prefix" "$STAGING/Library/Application Support/JackBridge/jack-prefix"
install -m 0755 "$INSTALLER/jack-prefix.sh"        "$STAGING/Library/Application Support/JackBridge/jack-prefix.sh"
# Stamp the prefix this package was built against into the shipped defaults.
# Runtime readers (jackd-launch via jack-prefix.sh, the Companion via
# JackTools.swift) take it from there, so the built and the driven JACK2 are
# the same one by construction.
sed -e "s|@JACK_PREFIX@|$JACK_PREFIX|g" "$INSTALLER/config.plist" > "$BUILD/config.plist"
install -m 0644 "$BUILD/config.plist" "$STAGING/Library/Application Support/JackBridge/config.plist.default"
install -m 0644 "$INSTALLER/launchagents/com.treefallsound.companion.daemon.plist" "$STAGING/Library/LaunchAgents/"
install -m 0644 "$INSTALLER/launchagents/com.treefallsound.companion.jackd.plist"  "$STAGING/Library/LaunchAgents/"
install -m 0644 "$INSTALLER/launchdaemons/com.treefallsound.companion.route.plist" "$STAGING/Library/LaunchDaemons/"
mkdir -p "$STAGING/Applications"
cp -R "$APP_BUILD_DIR/PiStompCompanion.app" "$STAGING/Applications/"

# Keep the GUI at /Applications. Without an explicit component plist,
# pkgbuild defaults the app bundle to relocatable, and PackageKit then asks
# LaunchServices where com.treefallsound.companion already lives. Every stale
# build-tree copy is a candidate, so an install silently lands the app on top
# of one of those instead of /Applications. Observed in install.log:
#   Applications/PiStompCompanion.app relocated to
#   Users/cam/dev/pistomp-companion/jackbridge/installer/build/staging/...
COMPONENT_PLIST="$BUILD/components.plist"
cat > "$COMPONENT_PLIST" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<array>
    <dict>
        <key>RootRelativeBundlePath</key>
        <string>Applications/PiStompCompanion.app</string>
        <key>BundleIsRelocatable</key>
        <false/>
        <key>BundleHasStrictIdentifier</key>
        <true/>
        <key>BundleIsVersionChecked</key>
        <true/>
        <key>BundleOverwriteAction</key>
        <string>upgrade</string>
    </dict>
</array>
</plist>
PLIST

echo "==> pkgbuild (component)"
COMPONENT_PKG="$BUILD/PiStompCompanion-component.pkg"
pkgbuild \
    --root "$STAGING" \
    --component-plist "$COMPONENT_PLIST" \
    --identifier "$PKG_ID" \
    --version "$VERSION" \
    --install-location / \
    --scripts "$SCRIPTS" \
    "$COMPONENT_PKG"

# Fail loud: a relocatable payload installs to the wrong path with no error.
# The signal is the <relocate> element in PackageInfo, NOT the pkg-info
# relocatable="false" attribute -- that attribute reads false either way.
#   opted out (what we want): <relocate/>
#   relocation on:            <relocate><bundle id="..."/></relocate>
rm -rf "$BUILD/verify-component"
pkgutil --expand-full "$COMPONENT_PKG" "$BUILD/verify-component" >/dev/null
if grep -q '<relocate>' "$BUILD/verify-component/PackageInfo"; then
    echo "error: component pkg opts in to bundle relocation." >&2
    echo "       PackageKit would install PiStompCompanion.app over whatever stale" >&2
    echo "       copy of $PKG_ID LaunchServices happens to know about," >&2
    echo "       instead of /Applications. Check the --component-plist above." >&2
    rm -rf "$BUILD/verify-component"
    exit 1
fi
rm -rf "$BUILD/verify-component"

echo "==> productbuild (distribution)"
DIST_XML="$BUILD/distribution.xml"
sed -e "s/@VERSION@/$VERSION/g" \
    -e "s|@JACK_PREFIX@|$JACK_PREFIX|g" "$INSTALLER/distribution.xml.in" > "$DIST_XML"

# Fail loud: an unsubstituted token becomes a literal path in the
# installation-check, which then fatals on every machine.
if grep -q "@[A-Z_]*@" "$DIST_XML"; then
    echo "error: unsubstituted token in distribution.xml:" >&2
    grep -o "@[A-Z_]*@" "$DIST_XML" | sort -u >&2
    exit 1
fi

PRODUCTBUILD_ARGS=(--distribution "$DIST_XML" --package-path "$BUILD")
if [[ -n "${SIGN_INSTALLER_IDENTITY:-}" ]]; then
    PRODUCTBUILD_ARGS+=(--sign "$SIGN_INSTALLER_IDENTITY")
fi
productbuild "${PRODUCTBUILD_ARGS[@]}" "$PKG_OUT"

if [[ -n "${NOTARY_PROFILE:-}" && -z "${SKIP_NOTARIZE:-}" ]]; then
    echo "==> notarize + staple"
    xcrun notarytool submit "$PKG_OUT" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$PKG_OUT"
else
    echo "==> skipping notarization (NOTARY_PROFILE unset or SKIP_NOTARIZE=1)"
fi

echo "==> Done: $PKG_OUT"
