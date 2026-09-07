# FW1814 integrated 48 kHz analog engine success

Date: 2026-09-07

This records the first successful production-style dynamic full-duplex analog path for the M-Audio FireWire 1814 on macOS, including clean CoreAudio playback and capture through the FW1814 HAL and hardware-validated automatic recovery after disconnect, reboot, and interface power cycling.

## Scope

The integrated engine runs the already-proven 48 kHz / internal-clock / S/PDIF-mode transport and exposes only hardware-confirmed analog I/O:

- CoreAudio-facing playback: Analog Outputs 1-4;
- CoreAudio-facing capture: Analog Inputs 1-8;
- S/PDIF stream positions remain hidden pending later cross-device verification;
- MIDI remains deferred;
- explicit headphone routing remains deferred.

## Startup

The integrated `fw1814analog48` engine successfully completed:

- documented M-Audio special mixer routing;
- full-duplex CMP connection;
- playback ISO start before capture ISO start;
- 50 ms ISO settle;
- correlated OUTPUT PLUG SIGNAL FORMAT CONTROL at 48000 Hz;
- 100 ms delay;
- correlated INPUT PLUG SIGNAL FORMAT CONTROL at 48000 Hz.

Observed startup result:

```text
FW1814 stream kick: PASS (matched INTERIMs=0, ignored unrelated=0)
FW1814 analog engine ONLINE
```

The correlated FCP layer is now used instead of accepting the first FCP response from the node. Matching INTERIM responses remain non-terminal and unrelated responses are ignored.

## Dynamic capture success

The production capture engine uses a 256-slot receive ring with metadata publication in 32-slot chunks. The earlier 64-slot geometry was useful during bring-up because it exactly matched the first hardware-proven raw duplex test, but the final production path returned to 256 slots once the real DBS=11 event-width bug was understood.

With the corrected 11-quadlet event width (10 PCM + 1 MIDI), the live capture path runs at 48 kHz.

Representative steady-state statistics while streaming:

```text
capture delta: 96000 frames / 2 s
rx-touched:    256/256
malformed:     0
invalid:       0
dbc-gap:       0
reorder:       0
stale:         0
```

Before a consumer attaches, the capture ring fills to capacity as expected. Once the test consumer begins issuing read calls, the producer activates live capture and the queue remains near empty while the consumer drains it.

### Physical Input 1 verification

With a steady signal injected into physical Analog Input 1, the CoreAudio-facing capture meter reported approximately:

```text
A1 ~= 0.1346
A2 ~= 0.00001
A3..A8 ~= 0.00003
```

This independently confirms the production decoder/remap path preserves the previously measured physical Input 1 mapping.

## Dynamic playback success

All four physical analog outputs were confirmed through the dynamic SHM -> PCM -> AMDTP path.

The physical order remains:

```text
Analog Output 1 <- raw PCM position 2
Analog Output 2 <- raw PCM position 3
Analog Output 3 <- raw PCM position 0
Analog Output 4 <- raw PCM position 1
```

A first temporary distortion during dynamic playback testing was traced to the SHM test producer: it wrote 240-frame bursts while the transport refilled 384-frame halves, causing zero-fill inside active tone periods. After buffering the producer ahead, a 3-second 500 Hz tone delivered exactly 144000 audio frames and sounded clean.

A second distortion remained for arbitrary audio. The important diagnostic pattern was frequency-dependent:

```text
500 Hz:    clean
437.5 Hz:  clean
440 Hz:    distorted
523.25 Hz: very distorted
Glass.aiff: distorted
```

The same distortion occurred with direct-SHM test tones, which excluded CoreAudio, the HAL, sample-rate conversion, source bit depth, channel interleaving, and level clipping.

The key observation was that 500 Hz and 437.5 Hz are phase-aligned with the full 128-packet loop duration, while 440 Hz and 523.25 Hz are not. This isolated the artifact to reuse of the short live TX NuDCL payload ring at the full-ring boundary.

### Final TX geometry fix

The original dynamic FW1814 playback geometry was:

```text
128 packets total
64-packet halves
16 ms full loop
8 ms per half
```

The released FW410 production 48 kHz engine already uses a much longer live TX ring:

```text
640 packets total
320-packet halves
80 ms full loop
40 ms per half
```

FW1814 was changed to the same 640/320 geometry while preserving all FW1814-specific blocking-mode packet rules, DBC, SYT, packet sizes, mixer state, AV/C controls and CMP behavior.

After this change, all three decisive playback tests were hardware-confirmed clean:

```text
440 Hz direct SHM:     clean
523.25 Hz direct SHM:  clean
Glass.aiff CoreAudio:  clean
```

Therefore the distortion was caused by the short 128-packet live payload-reuse interval, not by HAL/CoreAudio sample handling or the FW1814 protocol itself.

The validated production geometry is now:

```text
FW1814 playback TX ring: 640 packets / 320-packet halves (80 ms / 40 ms)
```

Do not reduce this geometry without a new arbitrary-frequency / real-audio regression test.

## CoreAudio HAL success

The first FW1814 AudioServerPlugIn is hardware-validated at fixed 48 kHz. macOS reports:

```text
M-Audio FireWire 1814
Input Channels:  8
Output Channels: 4
Current SampleRate: 48000
Transport: FireWire
Manufacturer: macfw
```

The HAL itself performs no FireWire access. It exchanges playback and capture through the FW1814 SHM ABI while `fw1814analog48` remains the sole FireWire owner.

Clean CoreAudio playback has been confirmed end-to-end:

```text
CoreAudio application
 -> FW1814 HAL
 -> playback SHM
 -> fw1814analog48
 -> blocking AMDTP
 -> FireWire
 -> physical analog output
```

Clean CoreAudio capture has also been confirmed from a normal recording application:

```text
physical analog input
 -> FireWire AMDTP capture
 -> fw1814analog48
 -> capture SHM
 -> FW1814 HAL
 -> CoreAudio recording application
```

Playback and capture remain clean together after the 640/320 TX-ring geometry fix.

## Automatic recovery success

The first automatic supervisor could recover device enumeration after disconnect, including guarded bootloader-to-operational recovery, but the recovered playback stream could intermittently return in a persistent broken device-side state. A service/engine restart without a FireWire bus reset did not clear that state.

A manual recovery sequence proved reliable:

```text
stop FW1814 launchd service
wait 3 s
perform guarded product-scoped FireWire bus reset
wait 3 s
start FW1814 launchd service
fresh init-48
start transport
```

The supervisor now implements the equivalent lifecycle automatically and safely:

```text
physical disconnect / reboot / interface power cycle
    -> recover bootloader/operational personality
    -> init-48 succeeds, proving the operational personality is ready
    -> perform one guarded product-scoped FW1814 bus reset
    -> consume the reset-required state before issuing the reset
    -> wait 3000 ms for re-enumeration
    -> run a fresh init-48 on the new generation
    -> start fw1814analog48
```

The reset helper matches only product `FW 1814`, opens the known device, calls `BusReset()`, and performs no further device transactions on the stale generation before closing.

The supervisor deliberately consumes its reset-required flag before issuing the reset, so the bus reset that it requested cannot recursively trigger another pre-transport reset.

Hardware validation after this change:

```text
disconnect/reconnect:   consistently returns clean
macOS reboot:           consistently returns clean
FW1814 power off/on:    consistently returns clean
playback after recovery: clean
capture after recovery:  clean
```

Recovery takes longer than the earlier immediate restart path because it intentionally includes the additional bus reset and re-enumeration settle period. The extra delay is accepted because it consistently returns the device to the known-good playback state.

This bus reset is therefore part of the validated FW1814 special-firmware lifecycle and should not be removed merely as a startup optimization without equivalent repeated disconnect/reboot/power-cycle hardware validation.

## Proven milestone

The FW1814 now has a hardware-proven dynamic 48 kHz full-duplex analog transport, working fixed-48k CoreAudio HAL integration, and reliable automatic lifecycle recovery:

- 4 verified analog playback channels;
- 8 verified analog capture channels;
- correct 48 kHz blocking AMDTP cadence;
- clean arbitrary-frequency playback with 640/320 TX geometry;
- production capture with 256-slot RX geometry;
- clean real CoreAudio playback;
- clean CoreAudio capture in a normal recording application;
- continuous 48 kHz capture;
- dedicated realtime audio-service scheduling;
- generation-safe teardown;
- correlated FCP rate controls;
- fixed-48k AudioServerPlugIn visible to macOS as a 4-out / 8-in FireWire device;
- automatic guarded bootloader recovery;
- automatic product-scoped clean bus reset before recovered transport start;
- hardware-validated recovery after disconnect/reconnect, macOS reboot, and FW1814 power off/on.

The fixed-48k analog path can now be treated as the stable FW1814 baseline. The next major transport task is native 44.1 kHz support. Its blocking cadence must be derived and validated independently rather than assuming the 48 kHz packet pattern. Digital exact L/R mapping, headphone source routing, and MIDI remain deferred as documented elsewhere.
