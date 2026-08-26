# Releases

Offline mirror of the release notes published on the
[treefallsound/pistomp-companion Releases page](https://github.com/treefallsound/pistomp-companion/releases).
The tree keeps the exact package contents and build procedure alongside the
release artifact.

## How to ship a release

Each release attaches the JACK2 prerequisite package and the signed,
notarized PiStomp Companion package. Install JACK2 first.

```bash
# 1. Build the jack2 fork package if the fork moved.
cd ../jack2 && ./build-macos-pkg.sh 1.9.22-sastraxi.N

# 2. Install JACK2 so the Companion package build can link against it.
sudo installer -pkg ../jack2/build/jack2-*.pkg -target /

# 3. Build the PiStomp Companion package.
cd ../pistomp-companion && ARCHS=arm64 ./jackbridge/installer/build-pkg.sh 0.X.0

# 4. Commit, tag, push, and create the release.
git add -A && git commit -m "v0.X.0: ..."
git tag v0.X.0 && git push origin main v0.X.0
gh release create v0.X.0 \
    --repo treefallsound/pistomp-companion \
    --title "PiStomp Companion v0.X.0 — ..." \
    --notes-file release-notes.md \
    ../jack2/build/jack2-*.pkg \
    jackbridge/installer/build/PiStompCompanion-0.X.0.pkg

# 5. Append a matching section to this file.
```

The release body for `gh release create` should be the same content as
the new section below — copy-paste, not a symlink, because the GitHub
release body is rendered as GitHub-flavored markdown and this file is
read as plain CommonMark.

---

## v0.2.0 — multicast-pin stack + fork-based jack2

Released 2026-06-28. [Release page](https://github.com/sastraxi/JackRouter/releases/tag/v0.2.0).

First release with the netJACK2 multicast-interface pin in place.
Required for the direct-cable pi-stomp setup.

### Install

Download and install both `.pkg` files, **jack2 first**:

1. **`jack2-1.9.22+sastraxi.5.pkg`** — JACK2 fork we depend on. Stock `jackaudio/jack2` 1.9.22 is missing the multicast-interface pin; without this fork, netJACK2's discovery times out on hosts with both wifi and a direct-cable NIC. Installs to `/usr/local`.
2. **`JackBridge-0.2.0.pkg`** — the HAL driver, the `JackBridged` daemon, the LaunchAgents, the route watcher, and the `jackd-launch` wrapper. Double-click and run. Trust the unsigned package (Right-click > Open) on first install.

Re-running the same `JackBridge-*.pkg` is safe — the postinstall preserves a hand-edited `config.plist` and only re-bootstraps the LaunchAgents.

### What's in this release

#### jack2 fork (3 commits on top of v1.9.22 + WAF backport from 1.9.23)

- `3a2f2488` — netadapter PI controller integrator reset on ringbuffer reset
- `719d833a` — `IP_ADD_MEMBERSHIP` / `IP_BOUND_IF` pin on the master's multicast group join via `JACK_NETJACK_MULTICAST_IF`
- `b3bfc408` — mirror pin on the slave's outgoing multicast `sendto()`

Built with `sastraxi/jack2/build-macos-pkg.sh`. Source at <https://github.com/sastraxi/jack2>.

#### JackBridge (v0.1.x → v0.2.0)

- **jackd-launch** sources `JACK_NETJACK_MULTICAST_IF` from `/var/run/jackbridge-route.iface`, so netmanager's setsockopt pin gets the right interface name without hardcoding in the LaunchAgent plist.
- **jackbridge-route-watcher** actively manages the two LaunchAgents (bootout+disable on disconnect, enable+bootstrap on wired) instead of relying on `KeepAlive+PathState`. Pairs bootout's SIGTERM with an explicit jackd pkill so the next install doesn't fail on "text file busy".
- **jb-is-wifi-iface**: extracted single source of truth for wifi classification; both `jb-detect-net-iface` and the route watcher source it.
- **LaunchAgent plists**: `KeepAlive=true`, `ThrottleInterval=2`, `WatchPaths` on `config.plist`. `RunAtLoad` intentionally absent.
- **pi/bin/jackbridge-pi-up**: reads `/run/jackbridge.iface` (set by `jackbridge-pin-route`'s `ExecStartPre`) as the source of truth for the iface name. Fixes the hardcoded `eth0` bug on boards with multiple wired NICs. Falls back to `$JACKBRIDGE_IFACE` then `eth0`.
- **pi/bin/jackbridge-pi-down**: iterates all `netadapter*` clients (not just `netadapter`), so stale instances from failed prior runs don't pile up and choke the audio thread.
- **installer/build-pkg.sh**: `ARCHS` env var to build a single-arch dev .pkg when the host's `JACK_PREFIX` is arm64-only. Universal release builds need a universal libjack at `JACK_PREFIX`.
- **jack-rebuild-mac.sh**: root-level dev script — rebuild jack2 from the fork and reinstall over the host's `/usr/local` install.

v0.1.x worked on single-NIC hosts by accident; v0.2.0+ is required for the direct-cable setup that drives the pi-stomp use case.

### Verification

After install:

```sh
/usr/local/bin/jackd --version
# 1.9.22 — version string is unchanged from upstream; the fork's
# commits are functional, not metadata.

strings /usr/local/lib/jack/netmanager.so | grep JACK_NETJACK_MULTICAST_IF
# should print: JACK_NETJACK_MULTICAST_IF
# If you see nothing, the jack2 fork .pkg didn't install (or the
# LaunchAgent has the old env-var-free environment).

jackbridge-ctl status
# Both LaunchAgents should be in "running" state. If jackd is "stopped",
# the route daemon has determined you're on wifi — plug in the cable.

jack_lsp | grep pistomp
# Once the cable is in, netadapter on the pi talks to netmanager on
# the Mac and the pistomp ports appear within ~5s.
```

### Known issues

- **Apple Silicon only (`arm64`).** Intel Macs are not supported; the
  install will fail on x86_64. The xcodeproj *can* produce universal,
  but the jack2 fork's `build-macos-pkg.sh` doesn't yet `lipo` an
  x86_64 build, so the resulting libjack is single-arch and
  `installer/build-pkg.sh` is forced to `ARCHS=arm64` to link. Intel
  is not a planned target — see the project policy in `CLAUDE.md`.
- **Unsigned + unnotarized**: the build skips signing unless
  `SIGN_APP_IDENTITY` / `SIGN_INSTALLER_IDENTITY` / `NOTARY_PROFILE` are
  set in the environment. macOS Gatekeeper will refuse to open the
  `.pkg` the first time — Right-click > Open to bypass.
- **No `jack2-` pkg on the Releases page for v0.1.x**: users on
  v0.1.x who don't have the fork's jack2 installed will hit
  `INADDR_ANY` discovery failure. `brew install sastraxi/jack2` is not
  a thing — the only way to get the fork's binary is via the
  `jack2-*.pkg` from this release or a later one.

---

## v0.1.x and earlier

The `madhatter68/JackRouter` upstream. No jack2 fork dependency (because
upstream 1.9.22's `INADDR_ANY` behavior happened to work on hosts with
a single NIC, which is the case the upstream project targets).
Discovery worked over wifi, with all the latency and reliability
problems that implies. The pi-stomp direct-cable use case was added
in this fork (v0.2.0); the old wifi path is no longer tested and
shouldn't be relied on — see `docs/architecture.md` for why.
