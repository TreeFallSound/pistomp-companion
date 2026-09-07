# Measuring the netadapter `-l` / `-g` pair

How to decide whether `NetLatency = 2` holds on this stack. Run it when you
have 30 minutes and a pedalboard you can leave running.

## Why

The pair is the largest tunable term in the monitoring trip.

| Pair | One way | Monitoring trip |
|------|---------|-----------------|
| L=4, G=1024 (current) | 1106 frames | 46.1 ms |
| L=2, G=512 | 722 frames | 30.1 ms |

The latency itself is arithmetic — `jb_one_way_latency_frames()` in
`jackbridge/shared/JackBridge.h` computes it. Only the stability needs a
measurement.

L=2 was recorded as unstable on 2026-08-30. That record is not usable. Two
defects that produce the same symptom were fixed after it:

- the cursor rework of 2026-08-31 (`1900d91`),
- the realtime ring-failure logging in the jack2 fork (`50a48017`, `e799bc04`),
  which amplified the rate it was measuring.

A third was fixed on 2026-09-06: the send cursor seeded against a stale HAL
head and parked up to a full ring ahead. Any measurement taken before that
date is void.

## Before you start

The pair is coupled. `NET_RING / 2` must exceed `NET_LATENCY * period`.

```sh
ssh pistomp@pistomp.local 'jack_bufsize'        # confirm the period, normally 64
```

At period 64: L=2 needs G >= 512 (256 > 128, holds). `jackbridge-pi-up` warns
to the journal if the inequality fails.

## Changing the pair

The Mac owns both values. Never edit `/etc/default/jackbridge` on the pi — the
next start overwrites it.

```sh
CFG="$HOME/Library/Application Support/JackBridge/config.plist"
/usr/libexec/PlistBuddy -c "Set :NetLatency 2" "$CFG"
/usr/libexec/PlistBuddy -c "Set :NetRing 512"  "$CFG"
jackbridge-ctl restart          # pushes to the pi and restarts both ends
```

The Settings window's **Pi tuning** section does the same thing.

After every restart, select the device again in the DAW. A stack bounce sets
`DeviceIsAlive` to 0 and a host that released the device does not re-acquire it.

## The runs

Three runs of 10 minutes, in this order:

| Run | Pair | Purpose |
|-----|------|---------|
| 1 | L=4 / G=1024 | Baseline on today's build. The old baseline is void. |
| 2 | L=2 / G=512 | The test. |
| 3 | L=4 / G=1024 | Confirms the difference came from the pair, not from drift. |

Keep the same pedalboard for all three. Keep signal running. The DSP graph is
the load, so it stays constant whatever you play.

## What to record

Run both panes. The failure is one-sided: a pair whose cushion overruns xruns
the **pi** while every Mac field stays clean.

```sh
just watch                      # both panes on a timer
```

Or separately:

```sh
jackbridge-ctl pi-status | grep -E 'xruns|net_restarts'
/usr/bin/log show --last 10m --info --debug \
  --predicate 'subsystem == "com.treefallsound.companion" AND category == "driver"' \
  --style compact | tail -20
```

Note `/usr/bin/log`, not `log` — zsh has a builtin of that name. Note
`--info --debug` — the health lines are `JB_LOG_INFO` and `log show` drops
them by default.

## Validity gates

Check these at the start and the end of every run. The transition counters
read clean through a 75 ms fault on 2026-09-06, so a run is void unless:

| Field | Required |
|-------|----------|
| `lead` | near 192, sawtoothing by one period. It must not park high. |
| `recvLag` | negative and stable. |
| `reanchorCount` | unchanged across the window. |
| `healthDeltaMax` | 0 |

A run that fails a gate measures the gate, not the pair. Discard it and repeat.

## Decision rule

Fix this before you look at the numbers.

L=2 passes only if all of these hold in run 2, and run 3 returns to run 1:

- pi `xruns_15m` = 0
- `starveFrames` = 0
- `nearMiss` = 0
- `daemonXruns` delta stays at the run 1 rate

Any pi xrun or any starved frame is a fail.

## What each outcome means

**L=2 passes.** The 2026-08-30 record was confounded. Adopt L=2 / G=512 and
save 16 ms of monitoring trip. Update `JB_NET_LATENCY_CYCLES` and
`JB_NETADAPTER_RING_FRAMES` in `jackbridge/shared/JackBridge.h` to match the new
`config.plist` defaults — they are the fallback for a region no daemon has
attached to yet, and must equal the defaults.

**L=2 fails.** Run L=2 / G=1024 next. That separates a cushion problem from a
ring problem, and it sets the multiplier `k` for deriving `-g` from `-l`
(`docs/plan-tuning.md`). If L=2 / G=1024 passes, the ring was the limit. If it
also fails, the cushion was, and L=4 keeps its floor.

## Results

| Date | Run | Pair | pi xruns 15m | starveFrames | nearMiss | daemonXruns | lead | Verdict |
|------|-----|------|--------------|--------------|----------|-------------|------|---------|
|      | 1   | 4/1024 |            |              |          |             |      |         |
|      | 2   | 2/512  |            |              |          |             |      |         |
|      | 3   | 4/1024 |            |              |          |             |      |         |
