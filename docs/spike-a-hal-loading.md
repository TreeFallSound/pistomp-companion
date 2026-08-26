# Spike A — HAL plug-in signing + loading pipeline

**Result:** PASS on Sequoia 15.7.2 / M1 Pro (2026-05-29). Tahoe + Developer ID chain deferred.

## What this proved

A HAL plug-in built with current Xcode (26.3), ad-hoc signed with hardened runtime, and dropped into `/Library/Audio/Plug-Ins/HAL/` loads cleanly under `coreaudiod` on macOS 15.7.2. The modernization of `jackbridge/driver/` can proceed: any load failure there is our bug, not an Apple-side regression.

BlackHole was used as a known-good canary so a failure would unambiguously point at the pipeline, not at a hand-rolled skeleton.

Key log line confirming load:
```
HALS_RemotePlugInRegistrar.mm:237 Attempting to load: BlackHole.driver
HALS_PlugInManager::LoadPlugIns:     Loading....
```
No `rejected` / `signature` / `validation` / `entitlement` errors against our bundle id. Device `BlackHole 2ch` appeared in `system_profiler SPAudioDataType`.

## Test matrix

| Machine | OS | Chip | Loads | In AMS | Sig errors |
|---|---|---|---|---|---|
| MacBookPro18,3 | 15.7.2 (24G325) | M1 Pro | yes | yes | none |
| _Tahoe TBD_ | 26.x | — | — | — | — |

## Deviations from the original plan

- **Ad-hoc sign (`codesign --sign -`) instead of Developer ID Application.** No Apple Developer account yet. The cert chain is exercised separately in Phase 1.3.
- **Sequoia-only.** Tahoe machine not on hand; verification deferred.
- **BlackHole `master`, not `develop`.** `develop` branch doesn't exist on the upstream repo.

## Reusables already in the tree

- `docs/macos-setup.md` — codesign incantation (line 87), install path (line 18), log predicate (line 112). All confirmed working as written.
- `spikes/A-hal-loading/` — `build-and-sign.sh`, `install.sh`, `verify.sh`. Throwaway, but kept for re-runs against Tahoe later.

## Reproduce

```
cd spikes/A-hal-loading
git clone https://github.com/ExistentialAudio/BlackHole.git blackhole
./build-and-sign.sh
sudo ./install.sh
./verify.sh
```

Cleanup:
```
sudo rm -rf /Library/Audio/Plug-Ins/HAL/BlackHole.driver
sudo killall coreaudiod
```

## Follow-ups

- Re-run on a Tahoe 26.x Apple Silicon machine when available.
- Re-run with Developer ID Application identity once an Apple Developer account is set up — folds into Phase 1.3.
