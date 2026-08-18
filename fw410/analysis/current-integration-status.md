# FW410 current integration status

Last updated: 2026-08-18

This document is the current handoff for the **CoreAudio integration and next-work phase**. It supersedes the CoreAudio/AudioDriverKit transition and next-work sections of `current-development-status.md`; the older document remains the detailed canonical record for reverse-engineering, packet formation, AV/C/CMP, mixer/topology, boot and historical experiments.

## Current result

macfw publishes **M-Audio FireWire 410** as a normal macOS CoreAudio device through a dependency-free AudioServerPlugIn. The device is visible in Audio MIDI Setup and the macOS audio selector and exposes native 44.1 and 48 kHz operation.

Hardware-backed playback is confirmed at both native rates:

- **44.1 kHz:** clear output using `AmdtpPcmStream44100` and the required FW410 post-start AV/C 44.1 reassertion.
- **48 kHz:** clear output with no SRC after enlarging the TX scheduler from the old 128/64-cycle geometry to 640/320 cycles.
- **rate switching:** `haltransport` has been hardware-confirmed switching 44.1 -> 48 -> 44.1 while running, selecting the matching native transport automatically.
- **full playback topology:** Logic Pro X hardware-validated all 10 CoreAudio output channels: Analog 1-8 and S/PDIF L/R.

The proven playback architecture is:

```text
macOS application
    -> CoreAudio
    -> macfw AudioServerPlugIn
    -> WriteMix Float32
    -> versioned shared-memory PCM ring
    -> rate-aware user-space macfw transport
    -> native rate-specific AMDTP scheduler
    -> CMP / NuDCL / IOFireWireLib
    -> M-Audio FireWire 410
```

The HAL callback does not own FireWire. It only copies real-time PCM to/from shared memory. AV/C, CMP, FireWire ISO, rate control, startup quirks and eventual bus-reset recovery belong in the companion transport/service layer.

## Playback topology

`analysis/stream-topology.md` is the topology source of truth.

At 44.1/48 kHz the FW410 host-playback stream contains 10 PCM/audio positions plus one MIDI slot. The raw FW410 AMDTP audio order is:

```text
position 1   S/PDIF 1
position 2   Analog 1
position 3   Analog 3
position 4   Analog 5
position 5   Analog 7
position 6   S/PDIF 2
position 7   Analog 2
position 8   Analog 4
position 9   Analog 6
position 10  Analog 8
position 11  MIDI
```

The CoreAudio-facing presentation intentionally uses the user-friendly physical order:

```text
CoreAudio 1   Analog Out 1   -> FW410 PCM position 2
CoreAudio 2   Analog Out 2   -> FW410 PCM position 7
CoreAudio 3   Analog Out 3   -> FW410 PCM position 3
CoreAudio 4   Analog Out 4   -> FW410 PCM position 8
CoreAudio 5   Analog Out 5   -> FW410 PCM position 4
CoreAudio 6   Analog Out 6   -> FW410 PCM position 9
CoreAudio 7   Analog Out 7   -> FW410 PCM position 5
CoreAudio 8   Analog Out 8   -> FW410 PCM position 10
CoreAudio 9   S/PDIF Out L   -> FW410 PCM position 1
CoreAudio 10  S/PDIF Out R   -> FW410 PCM position 6
```

The HAL/shared-memory playback ABI is version 3 and carries 10 interleaved Float32 output channels in this physical order. Both native transports explicitly permute that order into the FW410 AMDTP positions above.

Logic Pro X independently routed and hardware-validated every channel in this layout, including both S/PDIF channels. Browser/YouTube playback showed more occasional dropouts than Logic Pro, suggesting client/system buffering and scheduling contribute to the remaining jitter behavior.

## Native 44.1 kHz

`tools/transport/halbridge44100` is hardware-confirmed clean.

Required startup behavior on the tested FW410:

1. set both signal-format directions to 44.1 kHz;
2. create duplex CMP connections;
3. start duplex ISO with valid native 44.1 NODATA/data scheduling;
4. while streaming is live, reassert 44.1 kHz on both AV/C directions;
5. continue normal native 44.1 playback.

The post-start rate reassertion is an FW410/M-Audio startup quirk and must stay in the device transport state machine rather than the generic AMDTP packet generator.

The 44.1 status output reports the same `drops`, `late` and `silence` counters as the 48 kHz path.

## Native 48 kHz

`tools/transport/halbridge48000` is hardware-confirmed with clear native 48 kHz output.

The decisive A/B result was transmit scheduling margin:

```text
old / broken under HAL workload:
    TX ring:      128 cycles
    refill half:   64 cycles
    half window:   ~8 ms

new / clear:
    TX ring:      640 cycles
    refill half:  320 cycles
    half window:  ~40 ms
```

The committed 48 kHz path also uses a 16,384-frame PCM FIFO, drain-until-caught-up shared-memory consumption, user-interactive pthread QoS, a short callback-service interval, query-before-set rate behavior, and `lateCyclePolls`/inserted-silence diagnostics.

### Residual load-sensitive dropouts

Small dropouts can still occur when the Mac is busy with unrelated desktop work. Current evidence does **not** identify these as PCM starvation or scheduler-generated silence:

- `framesSilenced` remains zero when a dropout is heard;
- `lateCyclePolls` increases gradually but does not spike at dropout moments;
- the HAL producer remains at the expected 48 kHz cadence;
- enlarging the TX ring solved the major corruption, leaving only occasional load-sensitive glitches;
- Logic Pro X produced noticeably fewer glitches than browser/YouTube playback.

Treat this as unresolved user-space/client/FireWire servicing jitter below or beside the current high-level counters. Do not regress the proven 640/320 geometry while investigating it.

## Rate-aware transport supervisor

`tools/transport/haltransport` is hardware-confirmed as the unified runtime entry point for 44.1/48 playback.

It maps HAL shared state, reads the selected CoreAudio sample rate, starts the proven native rate-specific engine, watches for rate changes, and stops/restarts the matching engine through guarded CMP/ISO cleanup.

A 48 -> 44.1 transition exposed a useful lifecycle edge case: immediately after a rate change/re-enumeration, a first child attempt can temporarily observe a stale/transitional FireWire state such as node `0x0` and unknown AV/C rate. The supervisor retry then reacquires the normal node and succeeds. The eventual persistent service should explicitly wait for/reacquire a stable operational unit rather than relying on a failed child/retry cycle.

## Capture/input

The capture topology is known from `analysis/stream-topology.md`. At 44.1/48 kHz the incoming FW410 AMDTP stream is DBS=5:

```text
raw position 1  S/PDIF In L
raw position 2  Analog In 1
raw position 3  S/PDIF In R
raw position 4  Analog In 2
raw position 5  MIDI
```

The CoreAudio-facing order is:

```text
CoreAudio Input 1  Analog In 1
CoreAudio Input 2  Analog In 2
CoreAudio Input 3  S/PDIF In L
CoreAudio Input 4  S/PDIF In R
```

Capture is staged through a separate 4-channel Float32 shared ring:

- `hal/include/macfw_hal_capture_shm.h` defines the capture ABI;
- `tools/transport/capture_shared.h` decodes MBLA-24 samples and permutes raw positions into the CoreAudio-facing order;
- `tools/transport/capturebridge48000` establishes the required duplex 48 kHz ISO state and feeds the capture ring;
- `tools/transport/captureprobe` reports peak/RMS activity for Analog In 1/2 and S/PDIF L/R.

### 48 kHz capture transport hardware validation

The 48 kHz FireWire -> Float32 capture transport is hardware-confirmed.

After fixing NuDCL receive metadata updates, `capturebridge48000` produced steady two-second deltas around 95,000-97,000 decoded frames, consistent with the expected 48 kHz cadence. The decoder reported zero malformed packets and zero invalid MBLA labels.

A hardware signal applied to Analog Input 1 produced clear channel-separated activity in `captureprobe` while Analog Input 2 remained near the noise floor and S/PDIF stayed silent. One observed probe window reported approximately:

```text
Analog In 1  peak=1.000000  rms=0.186567
Analog In 2  peak=0.002982  rms=0.000589
S/PDIF L     peak=0
S/PDIF R     peak=0
```

Large `droppedFrames` values during standalone probing are expected because `captureprobe` is an occasional reader of a finite 32,768-frame ring while the producer runs continuously. They are not evidence of FireWire capture failure. In normal CoreAudio operation, `ReadInput` is intended to drain the capture ring continuously.

Important capture bring-up findings:

1. The receive ring must publish current NuDCL receive metadata on each cyclic pass. `AmdtpReceiveRing` now configures an update set so header/status/timestamp data remains current across ring revolutions.
2. The FW410 requires continuously valid host->device AMDTP traffic for sample-bearing capture. `capturebridge48000` therefore services the proven `AmdtpPcmStream48k` scheduler continuously with an empty 10-channel PCM FIFO, producing digital silence as duplex keepalive.
3. The capture shared-memory object is recreated cleanly between standalone bridge runs.

### CoreAudio input integration

The HAL now exposes a second stream object with 4 input channels and advertises `kAudioServerPlugInIOOperationReadInput`. `StartIO` maps the capture shared ring outside the hot I/O callback. `ReadInput` performs only lock-free shared-memory reads and zero-fills when capture is unavailable or under-runs.

The immediate validation target is **48 kHz CoreAudio recording** from Analog Inputs 1/2 and S/PDIF L/R. Native 44.1 capture has not yet been integrated into the normal transport and should be treated as pending even though the input stream advertises the device's global 44.1/48 kHz formats.

## Immediate sequence

1. Hardware-validate the new 4-channel CoreAudio HAL input stream at 48 kHz using `capturebridge48000` as the transport producer.
2. Integrate capture production into the normal rate-aware transport rather than using a standalone capture test process.
3. Add native 44.1 capture handling and preserve the 44.1 startup quirk in full-duplex operation.
4. Extract common 44.1/48 device/CMP/ISO lifecycle into a reusable transport core.
5. Move latency-sensitive transport servicing away from logging/control work and add finer jitter timing diagnostics.
6. Make transport startup automatic so a user does not manually launch a companion binary.
7. Add bus-reset, generation/node change, disconnect/reconnect and rate-change recovery.
8. Add mixer/routing/headphone and MIDI integration.
9. Finish packaging/release automation once the runtime is reproducibly installable.

## Bootloader / interface boot mode

Production startup must handle both FW410 personalities:

```text
FW Bootloader  model 0x00010058
FW 410         model 0x00010046
```

The repository already proves the bootloader -> operational transition from user space. When the companion transport becomes a persistent service, device startup must become a state machine that discovers the personality, performs the boot transition if required, waits for re-enumeration, reacquires fresh generation/node state, configures the requested clock domain, establishes CMP/ISO and recovers similarly after resets or reconnects.

Do not put boot handling in the HAL real-time callback. It belongs in the transport/device lifecycle service.

## Release direction

The agreed version format remains `x.yy.zzz`, with eventual tag-driven packages:

- `lite`: runtime/driver components only;
- `full`: runtime plus project tools;
- `source`: exact tagged repository `.tar.gz`.

See repository `RELEASES.md` for the release policy.
