# pi-stomp setup (device side)

What needs to be true on the Raspberry Pi for JackBridge to work. The Mac side is `macos-setup.md`; this doc is everything on the other end of the Ethernet cable.

## Hardware in the test rig

- Raspberry Pi running pi-stomp (Arch Linux ARM, kernel `6.18.x-rpi-rt-v8-rt` PREEMPT_RT).
- Onboard ALSA audio at `hw:0` (the pi-stomp HAT — what local jackd drives in normal operation).
- `end0` (built-in Ethernet) wired directly to the Mac. `wlan0` may also be up and provides the management path (`pistomp.local` mDNS, web UI).

## Networking

`pistomp.local` resolves to **`wlan0`** (DHCP from the LAN router), so ssh / mDNS / web UI all go over Wi-Fi by default. Audio must not — netJACK2 over Wi-Fi underruns even at modest buffer sizes (see `spike-b-clock-stability.md`).

With both ends of the direct Ethernet cable up and no DHCP server present, both Mac and pi will auto-assign IPv4 link-local addresses (RFC 3927, `169.254.0.0/16`) on their wired interfaces. In our 2026-05-30 test that produced:

| Side | Interface | Address |
|---|---|---|
| Mac | `en7` (Thunderbolt Ethernet) | `169.254.161.114/16` (auto, IPv4LL) |
| Pi  | `end0`                        | `169.254.125.193/16` (auto, IPv4LL) |

The pi's address will vary across boots — IPv4LL picks pseudo-randomly within `169.254.1.0`–`169.254.254.255`. Discover it from the Mac with `arp -an | grep en7` once the cable is up, or from inside the pi with `ip -4 addr show end0`.

### Finding the addresses each session

```bash
# Mac side — what's our wired IP?
ipconfig getifaddr en7

# Pi side (via wifi mgmt path) — what's the pi's wired IP?
ssh pistomp@pistomp.local 'ip -4 -o addr show end0 | awk "{print \$4}"'
```

If either side has no IPv4LL address (some configurations disable it), assign one manually in the same /16:

```bash
ssh pistomp@pistomp.local 'sudo ip addr add 169.254.10.2/16 dev end0'
```

This is ephemeral. Persisting it across reboots needs a `systemd-networkd` `.network` drop-in; not done yet.

### Quick reachability check

```bash
ping -c 3 -S <mac-wired-ip> <pi-wired-ip>   # ~0.5 ms, 0% loss expected
```

## Bringing up the netJACK2 slave

In normal pi-stomp operation, jackd runs as a systemd unit (`jack.service`, user `jack`) with the ALSA backend driving the pi-stomp HAT:

```
/usr/bin/jackd -t 2000 -R -P 75 -d alsa -d hw:0 -r 48000 -p 64 -n 2 -X seq -s
```

For JackBridge we need the same jackd topology *plus* netadapter loaded as an internal client, with all hardware↔netadapter port connections wired. The stock unit runs as user `jack` with `JACK_PROMISCUOUS_SERVER=jack`; cross-user client access from `pistomp` is fiddly enough that stopping the stock stack and re-running as `pistomp` is the path of least resistance (Spike B finding).

```bash
# 1. Stop the stock pi-stomp host stack (jackd + LV2 host + UI).
sudo systemctl stop mod-ala-pi-stomp mod-ui mod-host mod-amidithru jack

# 2. Start our own jackd (alsa master, same args as the stock unit but smaller -p).
nohup jackd -R -P 75 -d alsa -d hw:0 -r 48000 -p 128 -n 2 > /tmp/jackd-pi.log 2>&1 &
sleep 2

# 3. Load netadapter as an internal client, pinned to the Mac's wired IP.
MAC_WIRED_IP=169.254.161.114    # substitute your en7 address
jack_load netadapter -i "-C 2 -P 2 -a $MAC_WIRED_IP"

# 4. Wire hardware <-> netadapter inside the pi's JACK graph.
jack_connect system:capture_1   netadapter:playback_1
jack_connect system:capture_2   netadapter:playback_2
jack_connect netadapter:capture_1 system:playback_1
jack_connect netadapter:capture_2 system:playback_2
```

Naming convention reminder, because netadapter's port labels are slave-local: `netadapter:playback_*` is where you send audio *toward* the master (so `system:capture` feeds it), and `netadapter:capture_*` is what arrives *from* the master (so it feeds `system:playback`).

The Mac side wires *its* half of the graph automatically — JackBridge's daemon runs an auto-wire pass after activate and on every port-registration callback, so once the netadapter shows up on the master, `pistomp:from_slave_* → JackBridge #1:input_*` and `JackBridge #1:output_* → pistomp:to_slave_*` are established without any `jack_connect` on the Mac.

### Sanity checks

```bash
jack_lsp                      # should list system:*, netadapter:*
jack_lsp -c                   # all four connections from step 4 should be present
```

### Flag notes

- `-P 75`: matches Spike B. Lower priorities underflow the net master deadline on the Mac side.
- `-p 128`: matches `docs/macos-setup.md` default. Bump in lockstep with the Mac side if you want larger buffers — they must agree.
- `-C 2 -P 2`: 2-in / 2-out, matching the pi-stomp wet/DI use case scope.
- `-a $MAC_WIRED_IP`: pins netadapter to unicast to the Mac. See "Pinning netJACK2 to Ethernet" below.

## Teardown — restoring the pi-stomp stack

```bash
killall jackd
sudo systemctl start jack mod-host mod-ui mod-ala-pi-stomp mod-amidithru
```

Confirm `jack.service` came back up and pi-stomp's normal UI is reachable before walking away — the stack tolerates being stopped but the start ordering is fussy (`jack` must be `active` before `mod-host` will succeed). If `mod-host` is stuck in `activating` for more than ~15s, `systemctl reset-failed mod-host mod-ui mod-ala-pi-stomp` and re-start.

## Pinning netJACK2 to Ethernet

netJACK2 discovers via IPv4 multicast (default group `225.3.19.154`). With both `wlan0` and `end0` up, multicast follows whatever interface the OS routes the group through — by default that's the interface with the default route, i.e. Wi-Fi. We have to override.

The net backend's actual flags (from `jackd -d net --help` on JACK2 1.9.22):

- `-a, --multicast-ip` — *Multicast address, **or explicit IP of the master**.* Default `225.3.19.154`. **This is the pinning knob.** Set it to the Mac's wired IP and the slave talks unicast to that address; the kernel routes via `end0` because the `169.254.0.0/16` link-scope route on `end0` wins for that destination.
- `-p, --udp-net-port` — UDP port (default 19000). Rarely needs touching.
- `-M, --mtu` — MTU to the master (default 1500).
- `-C / -P` — number of audio in / out ports.
- `-i / -o` — number of *MIDI* in / out ports. **Despite the name, `-i` is not an interface flag.** Don't try to pin with it.

There is no interface-bind option in netJACK2. Pinning is purely via destination IP + the host routing table.

The Mac master (netmanager, loaded by `jackd-launch`) is a listener that binds on all interfaces — the slave's unicast destination is what determines which wire the traffic actually traverses; no symmetric flag is needed on the master side.

### Routing gotcha (uid-dependent route lookup)

On this pi's Arch setup, `ip route get <mac-wired-ip>` as `pistomp` (uid 1000) has occasionally returned the wlan0 default-gateway path; as root it correctly returns `dev end0`. Cause not nailed down (no `ip rule` entries explain it; possible per-uid systemd-networkd policy). The condition cleared after reboot in our test. If you see traffic on Wi-Fi when you expected Ethernet, check this first — `ip route get` from the user that runs netadapter.

### Verifying audio is on the wire

After both sides come up, with Wi-Fi still enabled on the Mac:

```bash
# On the Mac
sudo tcpdump -i en7 -n udp port 19000 -c 20
```

If you see UDP traffic between the Mac's en7 IP and the pi's end0 IP, you've pinned correctly. If you see nothing on `en7` but the JACK graph is alive, traffic escaped to Wi-Fi — re-check the `-a` value matches the Mac's `en7` address.

## RT scheduling caveat

This pi's stock kernel ships `CONFIG_RT_GROUP_SCHED=y`, and cgroup v2 has no `cpu.rt_runtime_us` interface — so every non-root cgroup has a zero RT-bandwidth budget and `sched_setscheduler(SCHED_FIFO)` returns `EPERM` from inside `system.slice` (and the launched-by-user shell session). jackd logs:

```
Cannot use real-time scheduling (RR/75) (1: Operation not permitted)
```

…and falls back to `SCHED_OTHER`. Audio still flows, but netadapter's ring buffer slips under load — you'll see `JackRingBuffer::Read : producer too slow` / `consumer too slow` in `/tmp/jackd-pi.log`, audible as occasional clicks/pops.

Fix paths (kernel-side; pi-stomp's image is being rebuilt without `RT_GROUP_SCHED` for the next release):

1. Boot with `systemd.unified_cgroup_hierarchy=0` → cgroup v1, where `cpu.rt_runtime_us` is settable per-slice.
2. Rebuild kernel without `CONFIG_RT_GROUP_SCHED`.

Until either lands, expect glitches and don't chase them in JackBridge — they're upstream of us.

## Future: coexisting with mod-host (wet + dry feeds to the DAW)

Today's setup stops the stock pi-stomp stack (`jack.service` + `mod-host` + `mod-ui` + …) and runs an isolated jackd as `pistomp` so netadapter can attach. That makes the device a clean "audio interface" — the DAW sees the raw hardware ins/outs — but it throws away the whole *point* of pi-stomp: the on-device LV2 effects chain. The user can't monitor through their pedalboard on the Mac, and the on-device output goes silent while JackBridge is active.

The better topology keeps the stock stack running and joins netadapter to it as a second client. The DAW then receives both feeds — direct hardware in (dry) and post-mod-host (wet) — and the on-device output keeps working unchanged. Users choose per-session whether they want to record dry and re-amp through pi-stomp later, record wet, or both.

**How it fits together:**

- `jack.service` keeps running. Don't stop it. mod-host's normal wiring (`system:capture_* → effects → system:playback_*`) is untouched, so the headphone/line out keeps producing the wet signal.
- netadapter is loaded as a client of the *same* jackd, from the `pistomp` user, using `JACK_PROMISCUOUS_SERVER=jack` to cross the user boundary (the stock unit sets this env var on the server side; clients need it set in their environment to find the socket). Spike B noted this was "fiddly but possible" — that's the fiddle to figure out.
- netadapter gets bumped from `-C 2 -P 2` to `-C 4 -P 4`. Pi-side wiring becomes:
  - `system:capture_1/2` → `netadapter:playback_1/2`  (dry feed → Mac)
  - `mod-host:output_1/2` → `netadapter:playback_3/4`  (wet feed → Mac)
  - `netadapter:capture_1/2` → `system:playback_1/2`  (return from Mac, summed with mod-host's existing playback wiring — JACK mixes multiple sources to the same port)
  - Mac side: JackBridge advertises 4 inputs. Auto-wire (`jackbridge/daemon/JackBridge.cpp`) handles whatever channel count netadapter exposes; the current `wire_direction` loop is already n-channel.

**Open design questions before implementing:**

- **Does the Mac return audio also flow through mod-host before hitting the speakers?** Two options: (a) sum directly into `system:playback` (what's drafted above — Mac playback bypasses pedalboard, mod-host's wet output mixes in alongside), or (b) sum into mod-host's input so the Mac return gets re-amped too. (a) is simpler; (b) is more interesting for reamp workflows. Maybe a routing-matrix UI exposes both.
- **Latency cost of monitoring through mod-host on the Mac.** Round-trip = pi → netJACK → Mac DAW → netJACK → pi → mod-host → DAC. If users want low-latency monitoring through pedalboard, they should monitor on the pi (via the headphone out) and just record dry on the Mac.
- **Sync to mod-ui's plugin state.** If the user changes a pedalboard preset while recording, the wet feed changes mid-take. Probably fine — that's the same constraint as any analog rig — but worth documenting.

## TODO / UX

Everything in "Bringing up the netJACK2 slave" is currently a manual shell ritual. The end-state shape for pi-stomp integration:

- A `pi-stomp-jackbridge.service` unit that runs *alongside* `jack.service` (no `Conflicts=`), loads netadapter into the stock jackd as `pistomp`, and applies whatever wiring matches the user's current routing config. The "stop the stack and run our own jackd" approach we use today is a Spike B compromise, not the target architecture — see the section above.
- Mac IP either configured via the pi-stomp web UI or auto-discovered (mDNS announce from the Mac side?).
- A routing-config interface on the device (touchscreen / web UI) so users choose: dry-only to DAW, wet-only to DAW, both, with/without Mac return going through pedalboard. Defaults to "both feeds, Mac return bypasses pedalboard" — the most flexible non-surprising option.
- An LCD/UI indicator that JackBridge is active so a user can tell at a glance whether the Mac is on the wire.
