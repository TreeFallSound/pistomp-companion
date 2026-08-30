# Next steps: decrease the network delay

Written 2026-08-29. Follows the status update in `docs/JITTER.md`.

## 1. The problem

The audio has clicks. The clicks occur approximately one time in each
2 seconds to 5 seconds.

Playback has more clicks than recording. Playback is the audio from the Mac to
the pi. Recording is the audio from the pi to the Mac.

The xrun counters do not count these clicks. The counters show 0 while the
clicks continue. Thus you must not use an xrun count to show that a change
corrects this problem.

## 2. The evidence

### 2.1 The delay is not symmetrical

Each test sent 30 ICMP packets on the direct cable. No packets were lost.

| Direction | Minimum | Average | Maximum | Jitter |
|-----------|---------|---------|---------|--------|
| Mac to pi | 1.121 ms | 2.386 ms | 7.517 ms | 1.335 ms |
| pi to Mac | 0.256 ms | 0.677 ms | 2.871 ms | 0.559 ms |

The JACK cycle is 1.333 ms at 48 kHz and 64 frames.

The round-trip time (RTT) from the Mac to the pi is 1.8 cycles as an average.
The maximum RTT is 5.6 cycles. The jitter is one full cycle.

### 2.2 The Mac and the cable are satisfactory

The pi-to-Mac test goes through the Mac USB network adapter two times. This
test gives a minimum RTT of 0.256 ms. Thus the adapter and the cable can
operate more quickly than the measurements in the other direction.

The slow direction is the direction in which the pi receives a packet and
replies to it.

### 2.3 The pi delays the packets

These are the conditions on the pi:

- `ethtool -c eth0` shows `rx-usecs: 49` and `tx-usecs: 49`. The driver holds
  each interrupt for a maximum of 49 microseconds.
- `/sys/class/net/eth0/threaded` is 0. NAPI operates in a softirq. The kernel
  has the PREEMPT_RT configuration. Thus a realtime thread stops the softirq.
- Interrupt 104 for `eth0` is on CPU0 only. The counter shows 15 112 255
  interrupts on CPU0 and 0 interrupts on the other CPUs.
- CPU0 also holds the jackd realtime thread TID 102163 (priority FF 70) and
  mod-host (priority FF 65).
- All 4 CPUs hold realtime threads.

### 2.4 The symptom agrees with the measurement

Playback is the direction in which the pi receives the audio. Playback has
more clicks. This agrees with paragraph 2.1 and paragraph 2.3.

The master-to-slave packets hold the playback audio. The slave-to-master
packets hold the recording audio. A late packet at the pi makes the netadapter
capture ring empty. The Mac cannot count this fault.

## 3. What we do not know

- We do not know the delay of the netJACK2 packets. The measurements use ICMP.
  The netadapter receives its packets on a realtime thread. ICMP does not.
- We do not know which counter, if any, counts the clicks.
- We did not test a Thunderbolt network adapter or a different cable.

## 4. The next steps

Do the steps in this sequence. Do one step at a time. Measure after each step.

### 4.1 Set a known configuration first

The best measured configuration is `-l 6` with `-g 2048`. The present
configuration is `-l 4` with `-g 1024`, which is worse.

Procedure:

    ssh pistomp@pistomp.local
    printf 'JACKBRIDGE_NET_LATENCY=6\nJACKBRIDGE_NET_RING=2048\n' | sudo tee /etc/default/jackbridge
    sudo systemctl restart pi-stomp-jackbridge

Caution: `NET_RING / 2` must be more than `NET_LATENCY x period`. If the value
is too small, the pi gets xruns and the Mac counters stay at 0.

### 4.2 Remove the interrupt delay

The driver holds each interrupt for a maximum of 49 microseconds in each
direction. Remove this delay.

Procedure:

    sudo ethtool -C eth0 rx-usecs 0 tx-usecs 0

Expected result: the RTT decreases by a maximum of 0.1 ms. This is a small
quantity. Do this step because it is easy, not because it is sufficient.

### 4.3 Move NAPI into a thread

This is the step with the highest expected effect.

NAPI operates in a softirq. On a PREEMPT_RT kernel, a realtime thread stops a
softirq. Thus the audio threads delay the packets. A thread has a priority.
A softirq does not.

Procedure:

    echo 1 | sudo tee /sys/class/net/eth0/threaded
    # then set the priority of the napi/eth0-* thread below the jackd threads
    sudo chrt -f -p 60 $(pgrep -f 'napi/eth0')

Expected result: the maximum RTT from the Mac to the pi decreases.

### 4.4 Separate the interrupt and the audio thread

Interrupt 104 and the jackd realtime thread TID 102163 are both on CPU0.

Put the interrupt on one CPU. Put the netJACK2 realtime thread on a different
CPU. Do not depend on the default assignment.

Note: `docs/JITTER.md` records an earlier test of this type. That test moved
the interrupt to CPU0 to separate it from a thread on CPU3. A realtime thread
is now on CPU0. Thus the two are together again.

### 4.5 Find a counter that counts the clicks

Paragraph 1 shows that the present counters do not count the clicks. Record
the ring occupancy of the netadapter on the pi. Compare this record with the
times of the clicks.

Use a known signal for this test. `docs/JITTER.md`, "What's left to try",
item 3 gives the method.

## 5. How to measure

Measure the RTT in both directions after each step:

    # Mac to pi
    ping6 -c 30 -i 0.2 <pi-link-local>%en7
    # pi to Mac
    ssh pistomp@pistomp.local 'ping6 -c 30 -i 0.2 <mac-link-local>%eth0'

Compare the maximum and the jitter, not the average.

Listen to the playback audio. This is the only reliable indication at this
time.

## 6. Related work

These items are correct but they do not decrease the delay:

- **The latency model gives the wrong value.** `JB_NET_LATENCY_CYCLES` is 2 and
  `JB_NETADAPTER_RING_FRAMES` is 512 in `jackbridge/shared/JackBridge.h`. The
  pi uses different values. The driver thus reports a latency that is too
  small, and the DAW aligns the recorded audio incorrectly. Correct the two
  constants, or send the values from the pi.
- **The driver does not publish a buffer size.** `SA_Device.cpp` does not
  answer `kAudioDevicePropertyBufferFrameSize` or
  `kAudioDevicePropertyBufferFrameSizeRange`. CoreAudio thus gives each client
  128 frames, and a DAW cannot request 64 frames. A buffer of 128 frames
  measured 5 times the xrun rate of a buffer of 64 frames.
- **The tuning values are on the device only.** `/etc/default/jackbridge` is
  in no repository. A new image removes it.
