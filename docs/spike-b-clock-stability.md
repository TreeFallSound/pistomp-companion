# Spike B — Config B clock stability (Pi netjack2 ↔ Mac CoreAudio)

**Result:** PARTIAL — smoke clean, formal 1hr capture deferred until Mac is on Ethernet. Found a real Phase-1 requirement on the way.

## What we tested

Config B topology, exactly as the architecture doc describes:
- **Mac:** `jackd -d coreaudio -d "Steinberg UR22C"` (single-clock backend) + `jack_load netmanager`. UR22C is the CoreAudio clock master.
- **Pi:** `jackd -d alsa -d hw:0` (IQaudIO codec) + `jack_load netadapter -i "-C 2 -P 2"`. Pi is the slave; netadapter does the cross-clock SRC between Pi's ALSA clock and the netjack2 transport.
- Sample rate 48 kHz, 1024-frame period, 2-cycle net latency, transport over WiFi (Mac side) → Ethernet (Pi side). Pi-stomp services (`mod-ala-pi-stomp`, `mod-ui`, `mod-host`, `jack`) stopped for the duration.

Signal: `jack_metro -f 880 -b 120` on Pi → `netadapter:playback_{1,2}` → over net → `pistomp:from_slave_{1,2}` on Mac → `jack_rec -d 30`.

## Result (2026-05-29)

| Run | Mac jackd RT prio | Mac xruns / 30s | Pi xruns / 30s | Capture |
|---|---|---|---|---|
| 1 (default) | 10 | **70** + slave disconnect | 0 | 30s WAV, audible degradation |
| 2 (matched Pi) | 75 | **0** | 0 | 30s WAV, clean |

## What this proves

The Mac side's default `jackd` realtime priority (`10`) is unusable — the netjack2 master client misses its deadline ~2.3×/sec and the slave connection actually drops mid-run. Bumping to `-P 75` (matching the Pi's setting) eliminates xruns entirely over 30s.

**This is a Phase 1.5 requirement, not a stylistic choice.** The LaunchAgent that starts `jackd` on Mac must specify `jackd -R -P 75 ...` (or higher). The `docs/macos-setup.md` example invocations should be updated to make this explicit.

## What this does *not* prove

- **Long-term clock stability.** 30s is a smoke test; the PLAN's pass criterion is "zero clicks in 1 hour." Drift caused by netadapter's internal SRC may only become audible over longer runs.
- **Ethernet-only stability.** The Mac was on WiFi for this run (no second Ethernet cable on hand). WiFi jitter is a confound — possibly the dominant one. The formal Config B claim must be retested with both endpoints on wired Ethernet.

## Status of the original pass criterion

> Zero clicks in 1 hour of capture. RMS-level continuity across the run.

Deferred. The 30s smoke says it's at least *plausible*; we need a wired Mac and a longer run to confirm.

## Reproduce

Mac side (pi-stomp services should be stopped on Pi first):
```
jackd -R -P 75 -d coreaudio -d 'AppleUSBAudioEngine:Yamaha Corporation:Steinberg UR22C:120000:1,2' -r 48000 -p 1024 &
sleep 2
jack_load netmanager
```

Pi side:
```
sudo systemctl stop mod-ala-pi-stomp mod-ui mod-host mod-amidithru jack
nohup jackd -R -P 75 -d alsa -d hw:0 -r 48000 -p 1024 -n 2 &
sleep 2
jack_load netadapter -i "-C 2 -P 2"
nohup jack_metro -f 880 -b 120 -A 0.3 &
sleep 1
jack_connect metro:120_bpm netadapter:playback_1
jack_connect metro:120_bpm netadapter:playback_2
```

Mac capture:
```
jack_rec -f capture.wav -d 30 pistomp:from_slave_1 pistomp:from_slave_2
```

Diagnostics:
```
# xruns
grep -c "XRun" /tmp/jackd-mac.log
ssh pistomp@pistomp.local 'grep -c "xrun" /tmp/jackd-pi.log'

# WAV sanity
sox capture.wav -n stat
```

Teardown:
```
# Mac
killall jackd
# Pi
ssh pistomp@pistomp.local 'pkill jack_metro; killall jackd; sudo systemctl start jack mod-host mod-ui mod-ala-pi-stomp mod-amidithru'
```

## Incidental findings (captured in docs)

- **`jackd -d coreaudio` does not hog the device.** With `jackd` bound to UR22C, system audio still played through UR22C alongside the JACK graph. Useful for dev (no need to switch system output away during testing) but a sharp edge for production. Documented in `idiosyncrasies.md`.
- **`jackd -d coreaudio -d <friendly name>` silently falls back to defaults.** The friendly name shown in Audio MIDI Setup (e.g. `"Steinberg UR22C"`) is not accepted; the long internal CoreAudio name from `jackd -d coreaudio -l` is required. jackd's fallback creates a default-in + default-out aggregate with cross-clock drift — exactly what Config B forbids — so the misconfiguration is easy and silent. Documented in `idiosyncrasies.md`.
- **Cross-user JACK access on the Pi is fiddly.** Pi-stomp's `jack.service` runs jackd as user `jack` with `JACK_PROMISCUOUS_SERVER=jack`. Even with `pistomp` in the `jack` group and the env var set, client tools could not find the socket — easier to just stop pi-stomp's services for testing and run our own jackd as `pistomp`. Not a blocker, but worth knowing before designing any "JackBridge daemon coexists with pi-stomp on the same Pi" scenario.

## Follow-ups

- **Get the Mac onto wired Ethernet, retest, run for 1 hour, analyze for drift.** Until done, Config B is not formally validated.
- **Add the RT priority requirement to `docs/macos-setup.md`** and bake it into the Phase 1.5 LaunchAgent (`com.jackbridge.jackd.plist`).
- Pi-stomp's stock `jack.service` already runs `jackd` as user `jack` with `LimitRTPRIO=infinity` and `JACK_PROMISCUOUS_SERVER=jack`. To use Pi-stomp's own jackd as the slave (rather than stopping it), we'd need to either (a) run the spike's clients as a member of the `jack` group with the right env, or (b) script the spike against a stopped pi-stomp. (b) is what was done here.
