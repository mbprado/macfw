# FW1814 project history

This file records visible project milestones rather than every diagnostic
experiment. Detailed protocol, transport and routing findings remain under
`devices/fw1814/analysis/`.

## 2026-09-10 — Analog input monitoring matrix validated

The complete analog half of the FW1814 `MIX_ANA_DIG_IN` register was validated
with known signals on all four physical input pairs. Each pair routed
independently to Mixer 1/2 and Mixer 3/4 with the documented low-byte values:

- Analog Inputs 1/2: `0x01` and `0x10`;
- Analog Inputs 3/4: `0x02` and `0x20`;
- Analog Inputs 5/6: `0x04` and `0x40`;
- Analog Inputs 7/8: `0x08` and `0x80`.

Every route was audibly confirmed on the requested mixer bus, unrelated buses
and host playback remained clean, and each route returned to the complete zero
value when disabled. The engine now establishes
`MIX_ANA_DIG_IN=0x00000000` at startup, exposes only the eight proven analog
cells and saves their typed differential controls through `fw1814state`.
Digital-input routing remains disabled.

The promoted Analog Inputs 1/2 -> Mixer 1/2 route was retained across a
launchd transport restart, both 48 -> 44.1 kHz and 44.1 -> 48 kHz transitions,
and physical disconnect/reconnect at 48 kHz. The analog-input matrix therefore
shares the validated routing-state lifecycle used by the earlier output and
headphone controls.

Logic Pro software monitoring was also compared with the direct hardware
monitoring path during this work. Capture through CoreAudio and playback on a
separate output pair operated correctly, with almost unnoticeable observed
latency compared with the direct signal.

## 2026-09-09 — Routing controls, persistence and headphone sources validated

The FW1814 gained its first end-user-style routing control surface. The active
transport remains the sole owner of the FireWire device; `fw1814ctl` sends
typed requests through the transport-owned local socket and never opens the
interface independently.

The initial writable subset covers two documented special-firmware registers:

- `MIX_STM_IN` for assigning Software Returns 1/2 and 3/4 to Mixer buses 1/2
  and 3/4;
- `SRC_ANA_OUT` for selecting the Mixer or AUX source independently for Analog
  Outputs 1/2 and 3/4.

These registers cannot be read back from the FW1814. The engine therefore
establishes a complete known baseline, keeps an authoritative software cache,
checks the FireWire generation around each write and updates the cache only
after a successful transaction.

Hardware testing first enabled Software Return 1/2 on both Mixer 1/2 and Mixer
3/4. The cached `MIX_STM_IN` value changed from `0x00000006` to `0x0000000e`,
audio played cleanly on Analog Outputs 1/2 and 3/4, and disabling the extra
route restored `0x00000006`. Selecting AUX for Analog Outputs 3/4 then changed
`SRC_ANA_OUT` from `0x00000000` to `0x00000002` without affecting Outputs 1/2;
selecting Mixer restored the original path.

Writable routing state is persistent. Successful typed changes are recorded
by `fw1814state` and replayed after the native engine reports ready. A custom
state containing `MIX_STM_IN=0x0000000e` and `SRC_ANA_OUT=0x00000002` survived:

- a launchd transport restart;
- 44.1 -> 48 kHz and 48 -> 44.1 kHz transitions;
- physical disconnect/reconnect at both supported rates.

`fw1814state reset` applies and records the proven straight-through macfw
baseline without claiming undocumented M-Audio factory-default semantics.

The next documented register, `SRC_HP_OUT`, was then exercised through a
guarded full-register diagnostic. Both physical headphone outputs followed
Mixer 1/2 with `0x00010001`; changing only the second field to Mixer 3/4
produced `0x00020001` and the two outputs followed their assigned mixer buses
exactly. This validated the headphone source bit layout and established
`0x00010001` as the engine startup baseline.

Individual cached controls were then enabled for both headphone outputs.
Headphone Output 2 retained its Mixer 3/4 selection (`0x00020001`) across a
launchd transport restart, while Headphone Output 1 retained its reciprocal
Mixer 3/4 selection (`0x00010002`) across a 48 -> 44.1 kHz engine transition.
Both returned independently to Mixer 1/2 and the complete cache returned to
`0x00010001`. The two validated mixer sources are now part of persistent state
and Reset Defaults. AUX selection remains deferred until its signal path is
established safely.

## 2026-09-08 — Native 44.1 kHz CoreAudio and dynamic rate switching validated

Native 44.1 kHz operation was added without sample-rate conversion. The
blocking AMDTP path uses the FW1814's alternating data/NODATA cadence rather
than assuming the fixed 48 kHz pattern. Hardware capture showed the expected
DBS=11, FDF=0x01 formation, continuous DBC advancement on data packets and
stable NODATA handling.

The decisive playback requirement was an 88,200-frame PCM-backed digital
silence preload before ISO setup. Several superficially equivalent warm-up
arrangements produced distortion; the validated sequence preloads two seconds
of silence and then lets it drain naturally through the ordinary audio service
loop. This ordering is now treated as part of the hardware contract.

All four analog outputs played cleanly with both 440 Hz and boundary-unfriendly
523.25 Hz tones. The latter remains an important regression test because it
exposes packet-ring boundary errors that phase-aligned tones can hide.

The supervisor and AudioServerPlugIn were then made rate-aware. Audio MIDI
Setup can switch the interface between native 44.1 and 48 kHz in either
direction; the supervisor tears down the old engine, performs the validated
recovery lifecycle, initializes the requested rate and starts the matching
native engine. Repeated tone tests and normal YouTube/CoreAudio playback were
clean at both rates. Physical reconnect restored the previously selected rate
and returned clean audio.

## 2026-09-07 — Full-duplex 48 kHz CoreAudio and automatic recovery validated

The production-style `fw1814analog48` engine connected the previously proven
transport to a dedicated FW1814 AudioServerPlugIn. macOS exposed the interface
as a normal CoreAudio device with four analog outputs and eight analog inputs.
Playback, recording and full-duplex operation were validated with normal audio
applications.

Early live playback was distorted even though source PCM and packet cadence
were correct. Expanding the transmit ring to 640 packets with 320-packet
refill halves provided approximately 40 ms of scheduling margin and made
arbitrary-frequency playback clean. Capture uses a 256-slot receive ring with
32-packet publication chunks and the confirmed 10-PCM-plus-MIDI event width.

The service path gained dedicated Mach-paced realtime audio servicing,
generation-safe teardown and a long-running launchd supervisor. A simple
restart was not sufficient after some reconnects because the interface could
return in a persistent broken device-side state. The reliable recovery
sequence became:

1. recover or boot the operational FW1814 personality;
2. perform a guarded product-scoped FireWire bus reset;
3. wait for re-enumeration;
4. initialize the new generation;
5. start a fresh transport engine.

Hardware testing validated clean recovery after FireWire disconnect/reconnect,
macOS reboot and FW1814 power off/on. The additional bus reset is therefore a
required part of the current special-firmware lifecycle, not an optional
startup optimization.

## 2026-09-06 — Analog channel maps and dynamic 48 kHz transport established

Known signals were injected one physical channel at a time to replace inferred
stream names with hardware evidence. The capture path confirmed eight analog
input positions in order, followed by two positions reserved for the digital
pair and the MIDI quadlet. The CoreAudio-facing analog input order is therefore
Analog Inputs 1 through 8.

Playback mapping confirmed all four physical analog outputs. The FW1814's raw
AMDTP PCM order differs from the desired user-facing order, so the transport
explicitly maps:

```text
Analog Output 1 <- raw PCM position 2
Analog Output 2 <- raw PCM position 3
Analog Output 3 <- raw PCM position 0
Analog Output 4 <- raw PCM position 1
```

Documented special-firmware routing connected Software Returns 1/2 and 3/4 to
Mixer buses 1/2 and 3/4, then selected those mixers for Analog Outputs 1/2 and
3/4. Dynamic shared-memory playback and capture endpoints were added around
the generation-safe duplex lifecycle, forming the basis of the later
production engine.

## 2026-09-06 — First successful FW1814 duplex AMDTP capture

The first successful transport required both CMP directions and real timed
host-to-device AMDTP traffic. Connecting only the device-to-host direction did
not make this special firmware transmit, and permanent playback NODATA was not
sufficient. Starting blocking silent playback before capture allowed the
documented OUTPUT control, 100 ms delay and INPUT control sequence to complete.

The device then filled every receive-ring slot with the expected 48 kHz
formation:

- host to device: 6 PCM + 1 MIDI, DBS=7;
- device to host: 10 PCM + 1 MIDI, DBS=11;
- eight events per data-bearing packet;
- NODATA packets held DBC and used their own DBS=2 header form.

This proved that macfw could start the FW1814 special-firmware duplex transport
and receive real device-to-host AMDTP data without issuing unsupported
BridgeCo stream-format commands.

## 2026-09-06 — Hardware identity and guarded boot lifecycle confirmed

Development began with read-only fingerprinting of the local FW1814 in both
bootloader and operational personalities. The observed BridgeCo hardware model
ID was `0x83`; configuration-ROM unit-directory values were `0x10070` for the
bootloader and `0x10071` for the operational device, matching the published
Linux FW1814 reference.

A guarded boot helper was added for the documented 12-byte boot-from-flash cue.
It requires the expected FW1814 product identity and BeBoB information block
before writing to `0xffffc8021000`. The interface re-enumerated successfully as
`FW 1814` after the cue.

The post-boot bus-reset helper initially exposed an important IOFireWireLib
lifecycle rule: `BusReset()` must be called only after `Open()`. With the
correct `Open() -> BusReset() -> Close()` sequence, the reset completed and the
bus generation advanced normally. These conservative identity and generation
rules became the foundation for every later mutating diagnostic and the
automatic recovery supervisor.
