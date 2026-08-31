# mod-host latency and the second input pair

The Mac device has four inputs. They are not the same signal, and they do not
carry the same delay.

| Mac input | Pi source | Extra delay |
|-----------|-----------|-------------|
| In1 / In2 | `system:capture_1/2` — the IQaudIO ADC, dry | none |
| In3 / In4 | `mod-monitor:out_1/2` — the mod-host plugin graph, wet | the graph's own delay |

`docs/LATENCY-MODEL.md` models the transport path only. It has no term for
mod-host. `jb_one_way_latency_frames()`
(`jackbridge/shared/JackBridge.h:314`) has no term for mod-host either. The
HAL advertises one number for the whole input scope
(`jackbridge/driver/JackBridge/Plug-In/SA_Device.cpp:1976`). That number is
correct for In1/In2 and too small for In3/In4.

This file answers three questions. Can we read the graph delay? Can CoreAudio
advertise it per stream? Is it worth building?

Read `docs/LATENCY-MODEL.md` first. This file sits beside it and uses its
symbols.

---

## 1. What the MOD stack exposes

### The wiring, verified on the live pi

`jackbridge/pi/bin/jackbridge-pi-up:199` wires the dry pair.
`jackbridge/pi/bin/jackbridge-pi-up:221` wires the wet pair. The live graph
matches. `jack_lsp -c` on the pi prints:

```
system:capture_1
   netadapter:playback_1
mod-monitor:out_1
   system:playback_1
   netadapter:playback_3
```

The pi runs `jackd -t 2000 -R -P 75 --port-max 512 -d alsa -d hw:0 -r 48000
-p 64 -n 2 -X seq -s`.

### LV2 `lv2:latency`

LV2 gives a plugin one way to report its own delay. The plugin declares an
output control port with `lv2:designation lv2:latency`, and writes the delay
in frames into that port. `/usr/lib/lv2/core.lv2/lv2core.ttl` on the pi
defines the term:

```
lv2:latency
	a rdf:Property ,
		owl:DatatypeProperty ;
	rdfs:range xsd:nonNegativeInteger ;
	rdfs:label "latency" ;
	rdfs:comment "The latency introduced, in frames." .
```

The declaration is optional, and most plugins do not make it. The pi holds 526
bundles in `~/.lv2`. Only 19 of them mention `lv2:reportsLatency` or the
`lv2:latency` designation. The pedalboard that is loaded now (`Looper_Kit`)
contains one such plugin, `urn:zamaudio:ZaMaximX2`, whose latency port has the
symbol `lv2_latency` (`~/.lv2/ZaMaximX2.lv2/ZaMaximX2_dsp.ttl:47-53`).

So a sum over the declared ports is a lower bound, not the graph delay. A
plugin that adds delay and declares nothing is invisible.

### mod-host

**mod-host does not handle latency at all.** VERIFIED: the binary
`/usr/bin/mod-host` on the pi contains zero occurrences of the string
`latency`, in any case. It contains the LV2 designation URIs it does use
(`lv2core#enabled`, `lv2core#freeWheeling`) and not `lv2core#latency`.

The text protocol has no latency command. The command list in the binary and
the command list in the upstream README agree, and neither has one
(https://github.com/moddevices/mod-host).

One indirect route exists. `monitor_output <instance> <symbol>` asks mod-host
to report an output control port. A client could subscribe to each plugin's
`lv2_latency` port and sum the values itself. We did not test this route. The
command port is busy — see below.

**The mod-host command port takes one client.** VERIFIED by observation. We
opened a second connection to `127.0.0.1:5555` and wrote `param_get 13
lv2_latency`. mod-host never read it. `ss -tnp` then showed our 26 bytes
sitting unread while mod-ui held the one established connection:

```
ESTAB      0      0    127.0.0.1:42352    127.0.0.1:5555
CLOSE-WAIT 26     0    127.0.0.1:5555     127.0.0.1:54870
```

Any Mac-side reader therefore has to go through mod-ui, or wait for mod-ui to
release the port. It cannot simply open its own socket.

### mod-ui

**mod-ui does not expose latency either.** VERIFIED: the string `latency` does
not appear in `mod/` or `modtools/` in the installed mod-ui 0.99.8, and
`modtools/libmod_utils.so` exports no JACK latency symbol. Its JACK surface is
`jack_get_buffer_size`, `jack_get_sample_rate`, `jack_cpu_load`,
`jack_port_get_all_connections` and similar. The route table in
`webserver.py:2402-2530` has no latency endpoint, on REST or on the websocket.

mod-ui listens on port 80 on this pi, not 8888.

### JACK's own latency API

JACK reports two ranges per port, through `jack_port_get_latency_range()`.
`jack_recompute_total_latencies()` makes the server walk the graph and refresh
them. This is the API a maintainer reaches for first, and on paper it is the
right one.

It does not work here, because the propagation depends on every client in the
chain implementing a latency callback. mod-host does not. VERIFIED with
`jack_lsp -l` on the live pi:

```
system:capture_1
	port capture latency = [ 64 64 ] frames
netadapter:playback_1
	port capture latency = [ 64 64 ] frames
netadapter:playback_3
	port capture latency = [ 0 0 ] frames
mod-monitor:out_1
	port capture latency = [ 0 0 ] frames
effect_12:outL
	port capture latency = [ 0 0 ] frames
```

The dry pair carries the ALSA figure through to the netadapter. The wet pair
reports zero at every hop. Every `effect_*` port reports zero. The graph delay
is not in there, and neither is the 64-frame ALSA capture that the wet pair
also traverses.

INFERRED: making this work needs a change to mod-host — a
`jack_set_latency_callback` that adds each plugin's reported latency into the
range. That is an upstream change, and it still only covers the 19 bundles in
526 that declare a latency port.

---

## 2. Per-stream latency on CoreAudio

### The two properties

CoreAudio has a device property and a stream property. A host is meant to add
them:

> The driver provides kAudioDevicePropertyLatency and
> kAudioStreamPropertyLatency, the sum of which represents the
> hardware-specific delay (in samples) between the audio hitting the mic and
> landing in the audio driver's buffer.

(Dan Klingler, coreaudio-api list,
https://www.mail-archive.com/coreaudio-api@lists.apple.com/msg01238.html)

So the mechanism exists. A driver with two input streams can give each stream
its own number.

### What this driver does today

The driver declares two stereo input streams and one stereo output stream
(`jackbridge/shared/JackBridge.h:360-362`). Stream 0 is In1/In2. Stream 1 is
In3/In4. `SA_Device.cpp:1306` derives each stream's starting channel from its
index, so the split is real and already visible to a host.

`kAudioStreamPropertyLatency` is implemented, and it always returns zero:

```
case kAudioStreamPropertyLatency:
	//	This property returns any additonal presentation latency the stream has.
	ThrowIf(inDataSize < sizeof(UInt32), ...);
	*reinterpret_cast<UInt32*>(outData) = 0;
	outDataSize = sizeof(UInt32);
	break;
```

(`SA_Device.cpp:1310-1315`.) The property is present in `HasProperty`
(`SA_Device.cpp:1139`), reports a size (`SA_Device.cpp:1211`), and is not
settable (`SA_Device.cpp:1167`). The value is a literal. It does not depend on
the stream, on the scope, or on shm. The driver never sends a change
notification for it.

`kAudioDevicePropertyLatency` carries the whole figure instead
(`SA_Device.cpp:803-813`), recomputed by `_UpdateAdvertisedLatency()`
(`SA_Device.cpp:1976-2008`) and notified at StartIO
(`SA_Device.cpp:2121-2127`).

To make stream 1 differ, we would add: a per-stream latency member; a read of
a new shm field; a `Host_PropertiesChanged` on `mInputStreamObjectID[1]`,
which already exists (`SA_Device.cpp:121`, `SA_Device.cpp:632`). That is a
small change. It is not the hard part.

### Do hosts honour it? Mostly no

This is the part that decides the question, and the answer is discouraging.

**JUCE reads stream 0 only.** VERIFIED in
`modules/juce_audio_devices/native/juce_CoreAudio_mac.cpp` on master.
`getStreamLatency (PlaybackDirection direction, int stream = 0)` takes the
default argument at both call sites, and the result feeds a single
device-wide figure:

```
int getInputLatencyInSamples()  final
{
    ...
    return aggregateDevice.getLatency (PlaybackDirection::input)
         + aggregateDevice.getSafetyOffset (PlaybackDirection::input)
         + aggregateDevice.getBufferSize()
         + aggregateDevice.getStreamLatency (PlaybackDirection::input);
}
```

(lines 867-876 and 2034-2043,
https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_devices/native/juce_CoreAudio_mac.cpp)

A JUCE host therefore never sees a value we put on stream 1. If we put the
graph delay on stream 0 instead, the host applies it to the dry pair, which is
wrong in the other direction.

**Ardour reads every stream and then throws the result away.** VERIFIED in
`libs/backends/coreaudio/coreaudio_pcmio.cc` on master. `get_latency()`
collects the per-stream values, reduces them to a maximum, and discards the
maximum under `#if 0` (lines 410-418). The collection loop is also miscoded:
it passes `kAudioDevicePropertyStreams` as the selector where it means
`kAudioStreamPropertyLatency` (line 366).

`AudioObjectPropertyAddress` also has one `mElement` field per query, so a
host that wants per-stream numbers must first enumerate
`kAudioDevicePropertyStreams` and then query each stream object. A host that
skips that step gets nothing, and most do.

INFERRED, from those two implementations: a per-stream latency value on this
driver would be read by almost nobody and acted on by fewer. Two open-source
CoreAudio backends, both widely used, both fail to use it correctly.

---

## 3. What a design would look like

Assume for this section that the number exists on the pi. Section 1 says it
does not, so this is a sketch, not a plan.

### The path

```
mod-host graph delay (frames)
   │  read on the pi
   ▼
jackbridge-pi-status                 key=value over ssh, read-only
   │  jackbridge-ctl pi-status
   ▼
Mac: the daemon (or the app)
   │  publish into shm
   ▼
shm: JB_OFF_MOD_LATENCY_FRAMES
   │  HAL reads at StartIO and on change
   ▼
kAudioStreamPropertyLatency on input stream 1
```

The transport already exists. `jackbridge/pi/bin/jackbridge-pi-status` is a
read-only key=value reporter, and `jackbridge-ctl pi-status`
(`jackbridge/tools/jackbridge-ctl:182`) already reads it over ssh. One more
key costs one line there.

The reader on the pi is the missing piece. Its options are: subscribe to each
plugin's `lv2_latency` port through mod-host `monitor_output`; or patch
mod-host to implement a JACK latency callback and then read
`jack_port_get_latency_range()` on `mod-monitor:out_1`. The second is the
correct one and the more expensive one.

The value changes whenever the user changes the pedalboard. So the field is
live state, not startup configuration, and the Mac has to poll it or be told.

### shm implications

Adding a field bumps `JACKBRIDGE_PROTOCOL_VERSION`
(`jackbridge/shared/JackBridge.h:45`, now 10). The bump forces a matched
rebuild of the driver, the daemon and `jbdump`, and a rebuild of the app,
which hand-copies the same offsets in
`app/PiStompCompanion/ShmReader.swift:169-196`.

**The 8-byte slot block before the device name is exactly full.** The atomic
fields run from `0x100` to `0x1f8`, and `JB_OFF_REANCHOR_COUNT` at `0x1f8`
(`jackbridge/shared/JackBridge.h:215`) is the last one.
`JB_OFF_DEVICE_NAME` starts at `0x200`
(`jackbridge/shared/JackBridge.h:230`). There is no gap between them. The
comment at `JB_OFF_END_FRAME_NUMBERS`
(`jackbridge/shared/JackBridge.h:160`) that says new atomic fields go there is
already out of date.

Space after the name is plentiful. The name takes 128 bytes, to `0x27f`. The
first ring buffer starts at `STRBUF_U0` = `0x10000`
(`jackbridge/shared/JackBridge.h:371`). So `0x280` to `0xffff` is free —
65408 bytes, or 8176 more 8-byte slots. A new field goes at `0x280`, and the
`static_assert` chain (`jackbridge/shared/JackBridge.h:394-411`) extends to
cover it.

So the layout cost is small. The version bump is the real cost, because it
takes every component with it.

---

## What we do not know

- Whether `monitor_output` on the `lv2_latency` port works as we expect. We
  did not test it. The command port was held by mod-ui.
- Whether mod-host serves one client by design or refused ours for another
  reason. We observed the unread bytes; we did not read the source.
- What Logic, Live, Pro Tools, Reaper, Bitwig and Studio One do with
  `kAudioStreamPropertyLatency`. They are closed. JUCE and Ardour are the only
  implementations we read.
- The real graph delay of any pedalboard. We never obtained a number, from any
  route.
- Whether the wet pair's missing 64-frame ALSA capture latency (section 1)
  matters to anything today. Nothing on the Mac reads the pi's JACK ranges.
- Whether `mod-monitor` itself adds a period. It is a JACK client between the
  graph and the netadapter, and it reports zero, like everything else in that
  chain.

---

## Recommendation

**Do not build per-stream latency.**

Three reasons, in order of weight.

1. **The number does not exist.** mod-host does not compute a graph delay, does
   not report one, and does not propagate JACK latency ranges. Every route to
   the number starts with an upstream change to mod-host. Even after that
   change, only 19 of the 526 installed bundles declare a latency port, so the
   result is a lower bound.
2. **Hosts would not use it.** JUCE reads stream 0 and ignores stream 1.
   Ardour reads every stream and discards the result. The CoreAudio mechanism
   is real, and the ecosystem does not use it.
3. **The driver change is the cheap part.** Adding the property costs a few
   lines. Adding the shm field costs a protocol bump and a four-component
   rebuild. Both are affordable, and both would deliver a number that nothing
   asks for.

Do this instead, if the delay becomes a real complaint:

- **State it.** In1/In2 carry the advertised latency. In3/In4 carry that plus
  the pedalboard's own delay, which JackBridge does not measure. That belongs
  in `docs/LATENCY-MODEL.md` as a named gap, so the next reader does not treat
  the advertised figure as covering all four inputs.
- **Show it, do not advertise it.** If we ever get the number, put it in the
  Settings window or the menu bar, where a person reads it and compensates by
  hand. A displayed number that is a lower bound is honest. An advertised
  number that half the hosts ignore is not.
- **Keep `kAudioDevicePropertyLatency` where it is.** It is correct for the
  dry pair. Moving it to split the difference would make it wrong for both
  pairs.
