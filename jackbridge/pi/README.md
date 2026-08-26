# JackBridge — pi side

Turns the pi-stomp into a 4-in / 2-out audio interface for a Mac DAW over Ethernet (netJACK2). The performance-pedal stack (`jack.service` + `mod-host` + `mod-ui`) keeps running. This is a *second*, on-demand JACK client.

## Turning it on and off

The LCD UI is the supported control path: open the menu, find "Ethernet Audio Interface", toggle it. Same flow from a shell:

```sh
sudo systemctl start  pi-stomp-jackbridge.service   # recording mode on
sudo systemctl stop   pi-stomp-jackbridge.service   # off (default state)
systemctl is-active   pi-stomp-jackbridge.service
```

Intentionally **not** enabled at boot — netadapter's encode/decode is real CPU cost and isn't free for a performance pedal. Start it only when you want to record.

## What the DAW sees

| DAW input  | Source on the pi                                           |
|------------|------------------------------------------------------------|
| In1, In2   | Raw hardware capture — guitar pre-pedalboard (reamp source) |
| ModOut1/2  | Post-mod-host wet — same audio as the headphone jack       |

| DAW output | Goes to                                                    |
|------------|------------------------------------------------------------|
| Out1, Out2 | Summed into `system:playback_*` alongside mod-host         |

## When it doesn't work

```sh
sudo journalctl -u pi-stomp-jackbridge.service -f      # service's own log
sudo journalctl -u jack -f | grep RingBuffer            # live xruns
JACK_PROMISCUOUS_SERVER=jack jack_lsp -c | grep -E 'netadapter|system'
cat /run/jackbridge.iface; ip route get 225.3.19.154   # multicast pin
```

xrun history (the LCD's 1m/5m/15m counters read this — one Unix-epoch timestamp per line, capped to the last 15 min, atomic rewrite):

```sh
cat /tmp/pi-stomp-jackbridge.xruns
```

Expected JACK graph once up: `netadapter:capture_{1,2}` (Mac → pi), `netadapter:playback_{1..4}` (pi → Mac), wired to `system:*` + `mod-monitor:*`.

If the Mac doesn't see the pi: it's almost always the multicast pin landing on the wrong NIC. Check `/run/jackbridge.iface`.

## Tunables

### Network interface (`JACKBRIDGE_IFACE`)

The service pins netJACK2's multicast group (`225.3.19.154`) to one NIC before loading netadapter. Detector preference:

1. `$JACKBRIDGE_IFACE` if wired and carrier=1.
2. A wired iface with a `169.254.0.0/16` address (direct Mac↔pi cable).
3. Any other wired iface with carrier + IPv4.

**Wired only** — there is no Wi-Fi fallback; 4ch/48k over wireless just produces mystery xruns. No wired iface = service start fails loudly.

Override:

```sh
# /etc/default/jackbridge
JACKBRIDGE_IFACE=eth1
```

Then `sudo systemctl restart pi-stomp-jackbridge.service`. The unit reads this via `EnvironmentFile=-/etc/default/jackbridge`.

### Sample rate / period / latency (not ours)

netadapter is an internal client of the stock pi-stomp `jackd` and inherits its rate, period, and clock. We don't set them. Image defaults are 48 kHz / 128 frames / 2 periods.

To change: edit `/lib/systemd/system/jack.service`'s `ExecStart=` line, then `systemctl daemon-reload && systemctl restart jack`, then restart this service. Raising the period for headroom raises it for the performance pedal too — that's how mod-host gets its audio.

Round-trip on direct Ethernet at 128/2/48k is ~20 ms — see `docs/spike-c-latency-results.md` for the full matrix (numbers there are authoritative).

### Channel wiring

`jackbridge-pi-up` does the post-load `jack_connect`s. Edit it if you want a different topology (e.g. send dry to ModOut1, or feed a specific `effect_NN:out` to In3/In4) and `systemctl restart`. The mod-host wet port name varies across builds (`mod-monitor:out_*` vs `mod-host:monitor-out_*`); first match wins, candidate list is at the top of `wire_wet`.

## Installed layout

```
/usr/lib/systemd/system/pi-stomp-jackbridge.service
/usr/local/libexec/jackbridge/jackbridge-pi-up          # ExecStart
/usr/local/libexec/jackbridge/jackbridge-pi-down        # ExecStop
/usr/local/libexec/jackbridge/jackbridge-xrun-watcher   # foreground process
/usr/local/libexec/jackbridge/jb-detect-net-iface
/usr/local/libexec/jackbridge/jackbridge-pin-route      # ExecStartPre=+
/usr/local/libexec/jackbridge/jackbridge-unpin-route    # ExecStopPost=+
/etc/default/jackbridge                                 # optional env file
/tmp/pi-stomp-jackbridge.xruns                          # runtime
/run/jackbridge.iface                                   # runtime
```

## Building / installing from source

```sh
git clone https://github.com/treefallsound/pistomp-companion
sudo bash pistomp-companion/jackbridge/pi/install.sh
```

Idempotent. Stages files only; does **not** enable the service. The pistomp-arch image build invokes this same script at bake time.

## Not covered here

- **netJACK2 on the wire.** Upstream JACK2 docs.
- **The Mac side.** Repo root `README.md`, `docs/macos-setup.md`.
- **The LCD code.** pi-stomp repo; menu integration spec lives at `../pi-stomp/JACKBRIDGE_RECORDING.md`.
- **Mac daemon ↔ HAL IPC.** `docs/architecture.md`. Not relevant to the pi side.
