# Plan B — The daemon stays alive when jackd stops

Written in ASD-STE100 Simplified Technical English.

## Status

Not started. Change A is complete. Change A is in `SA_Device.cpp`.

## The problem

`JackBridged` stops when jackd stops. The `on_shutdown` callback in
`jackbridge/daemon/JackBridge.cpp` sends `SIGTERM` to the daemon. The
LaunchAgent `KeepAlive` starts the daemon again.

The daemon writes the heartbeat in the JACK process callback. Thus no jackd
means no heartbeat. The heartbeat stops for the full restart time.

The user hears silence for this time. The silence is longer than the cable
fault. Each restart can also fail. A failed restart makes the user click
**Restart JackBridge**.

## The goal

The daemon must stay alive when jackd stops. The daemon must connect again
when jackd starts.

After this change:

- The heartbeat does not stop when jackd stops.
- The silence continues only while the audio link is down.
- The daemon does not use the LaunchAgent restart for a cable fault.

## The changes

### 1. Move the heartbeat out of the JACK callback

The heartbeat is in the process callback. Move it to a monotonic timer
thread in the daemon.

Write the heartbeat every 5 ms or less. The HAL declares a stall after
approximately 5 ring buffers.

Do not write the heartbeat from the JACK callback again. The heartbeat must
show that the daemon is alive. The heartbeat must not show that jackd
cycles.

### 2. Do not stop in `on_shutdown`

Remove the `kill(getpid(), SIGTERM)` call from `on_shutdown`.

Do these steps in `on_shutdown` instead:

1. Set `shmDriverStatus` to `JB_DRV_STATUS_INIT`.
2. Set `shmSlavePortsConnected` to 0.
3. Set the shm fault bit for the stall.
4. Release the JACK client handle.
5. Tell the reconnect loop to start.

Keep the heartbeat running. Keep the shm attachment open.

### 3. Add a reconnect loop

Add a loop in the daemon main thread. The loop calls `jack_client_open`
again. Use an interval of 2 s. Do not use an exponential backoff. The user
waits for audio.

Do these steps after a successful connect:

1. Register the ports again.
2. Wire the graph again.
3. Publish the JACK timing values again.
4. Publish the device name again.

The HAL must not see a protocol change. Do not change the shm layout. Thus
do not increment `JACKBRIDGE_PROTOCOL_VERSION` for this change.

### 4. Keep the loud failures loud

Rule 4 in `CLAUDE.md` stays true. Stop the daemon for these conditions:

- The shm protocol version does not agree.
- The shm attachment fails.
- jackd reports the wrong backend.

A cable fault is a transient condition. A transient condition is not a
failure. Do not stop the daemon for a transient condition.

## Risks

**Stale ring buffer.** The ring buffer holds old audio after a reconnect.
Clear the ring buffer before you activate the JACK client.

**Two daemons.** The LaunchAgent can start a second daemon. Make sure that
only one daemon attaches. Use the existing instance field in shm.

**A silent daemon.** The daemon stays alive but does not connect. The user
sees a green icon and hears nothing. Prevent this condition: the app shows
`.noAudioFromPi` from the shm fault bit. Verify this behaviour.

## How to test

1. Play audio in a DAW.
2. Disconnect the Ethernet cable.
3. Make sure that the DAW keeps the device.
4. Connect the cable again.
5. Make sure that audio starts again. Do not touch the DAW.
6. Make sure that the daemon process ID does not change.

Step 6 is the test for this plan. Step 3 is the test for change A.

## Related

- Change A: `SA_Device.cpp`, `kAudioDevicePropertyDeviceIsAlive`.
- `CLAUDE.md` section 4b — recovery has two audiences.
- `CLAUDE.md` section 4, rule 4 — fail loud, not silent.
