# FW410 current integration status

Last updated: 2026-08-19

This is the current handoff for CoreAudio integration. The older `current-development-status.md` remains the detailed historical record for boot, AV/C/CMP, packet formation, mixer/topology and the earlier bridge experiments.

## Executive status

macfw publishes **M-Audio FireWire 410** as a normal macOS CoreAudio device through an AudioServerPlugIn.

Hardware-backed integration confirmed on the tested Intel Mac:

- native 44.1 and 48 kHz playback;
- automatic 44.1/48 playback-engine selection through `haltransport`;
- ten CoreAudio playback channels validated in Logic Pro: Analog 1-8 and S/PDIF L/R;
- four CoreAudio input channels exposed: Analog 1/2 and S/PDIF L/R;
- native 48 kHz recording validated in Logic Pro;
- controlled 1 kHz capture with no significant steady-state discontinuities after completed-chunk receive handling.

The current production direction is a user-space split:

```text
CoreAudio application
    -> macfw AudioServerPlugIn
    -> versioned shared-memory playback/capture rings
    -> macfw companion transport/service
    -> AV/C / CMP / NuDCL / AMDTP
    -> M-Audio FireWire 410
```

The HAL callback owns only real-time-safe shared-memory transfer. FireWire and device lifecycle remain outside the callback.

## Playback

### CoreAudio-facing order

1. Analog Out 1
2. Analog Out 2
3. Analog Out 3
4. Analog Out 4
5. Analog Out 5
6. Analog Out 6
7. Analog Out 7
8. Analog Out 8
9. S/PDIF Out L
10. S/PDIF Out R

The shared playback ABI is version 3 and carries ten interleaved Float32 channels in this physical order. Native transports permute that order into the FW410 AMDTP stream positions documented in `stream-topology.md`.

### 44.1 kHz

Clear native 44.1 playback is hardware-confirmed. The tested FW410 requires the M-Audio post-start AV/C reassertion:

1. select 44.1 kHz in both directions;
2. establish duplex CMP/ISO;
3. start valid native AMDTP;
4. reassert 44.1 kHz on both AV/C directions while streaming is live;
5. continue normal playback.

This belongs in the FW410 device startup state machine, not the generic AMDTP scheduler.

### 48 kHz

Clear native 48 kHz playback is hardware-confirmed. The decisive scheduling change was increasing TX geometry from 128 cycles/64-cycle halves to 640 cycles/320-cycle halves. The committed path also uses a 16,384-frame PCM FIFO and continuous service of the native scheduler.

Some client/load-sensitive playback glitches remain possible, particularly outside Logic Pro. Existing diagnostics have not proven these to be PCM starvation or scheduler-inserted silence. Preserve the validated 640/320 geometry while investigating.

## Capture

The complete capture design and validation record is in [`capture-pipeline.md`](capture-pipeline.md).

### CoreAudio-facing order

1. Analog In 1
2. Analog In 2
3. S/PDIF In L
4. S/PDIF In R

The raw `DBS=5` FW410 order is S/PDIF L, Analog 1, S/PDIF R, Analog 2 and MIDI. `capture_shared.h` decodes MBLA-24 samples and permutes the four audio positions into the CoreAudio order.

### Shared capture ring

The capture ABI uses `/macfw_fw410_capture_v2` with four interleaved Float32 channels and 32,768-frame capacity. It also carries producer/decode counters and HAL `ReadInput` consumption counters.

`ReadInput` performs lock-free ring reads and zero-fills only when capture is inactive or genuinely under-runs.

### Prefill

The bridge waits for observed HAL `ReadInput` activity and accumulates 4,096 frames before setting the capture ring active. This controlled approximately 85 ms cushion removes startup starvation while keeping the steady-state queue near the live edge.

### Completed receive chunks

The first end-to-end recordings proved the FireWire-to-HAL path but sounded broken even with a healthy shared ring. The cause was receive publication granularity and temporal consistency, not CoreAudio routing.

Validated solution:

- NuDCL receive metadata is published in 32-cycle groups, approximately 4 ms;
- userspace waits for the terminal slot of a group to publish a changed signature;
- only that completed 32-slot group is snapshotted;
- DBC continuity is checked and ordering is applied inside the completed group;
- arbitrary cross-generation scanning of all 256 receive slots is avoided.

Representative final run:

```text
capture frames=940064 (delta 96000)
active=1 queued=3968
drops=0 malformed=0 invalid=0
chunks=4981
dbc-gap=0 ts-back=4 reorder=0 stale=0
```

The controlled 1 kHz recordings improved from hundreds of discontinuity clusters to one startup-region event and no significant steady-state discontinuities.

Small `ts-back` increments remained but did not correlate with audible or sample-level failures. They are currently diagnostic only.

## Duplex requirement

FW410 capture becomes sample-bearing only while valid host-to-device AMDTP is also flowing. The standalone capture bridge therefore runs the proven 48 kHz playback scheduler with an empty ten-channel PCM FIFO, producing timed digital silence as keepalive.

The next runtime must replace this keepalive with the real CoreAudio playback ring while preserving identical duplex timing.

## Current tools

- `haltransport` — rate-aware 44.1/48 playback supervisor
- `halbridge44100` — proven native 44.1 playback engine
- `halbridge48000` — proven native 48 kHz playback engine
- `capturebridge48000` — proven 48 kHz capture plus playback keepalive
- `captureprobe` — capture channel activity diagnostic
- `shmprobe` — shared-ring and HAL I/O diagnostics
- `fwboot` — guarded bootloader-to-operational transition

## Boot and lifecycle

A persistent service must handle both personalities:

```text
FW Bootloader  model 0x00010058
FW 410         model 0x00010046
```

Production startup must discover the personality, boot when necessary, wait for re-enumeration, reacquire fresh generation/node state, configure the selected clock domain and establish duplex CMP/ISO.

Bus resets, rate changes and reconnects must be treated as normal recoverable lifecycle events. None of this work belongs in the HAL real-time callback.

## Immediate sequence

1. Merge validated 48 kHz capture into the normal rate-aware transport.
2. Replace the capture bridge's silence keepalive with real ten-channel playback.
3. Validate simultaneous playback and recording in Logic Pro under sustained load.
4. Add native 44.1 capture using the same completed-group receive discipline and the required FW410 rate-reassertion sequence.
5. Extract common device/CMP/ISO lifecycle into a persistent service core.
6. Add automatic boot, bus-reset, generation-change and reconnect recovery.
7. Return to mixer/routing/headphone and MIDI integration.
8. Finish install/packaging and tag-driven release automation.

## Architecture rules to preserve

- Keep HAL, device control and FireWire transport separated.
- Never perform FireWire/control work in the real-time HAL callback.
- Preserve hardware-validated NuDCL behavior during refactors.
- Treat completed receive groups as the capture publication unit.
- Preserve exact PCR state and guarded cleanup.
- Prefer native sample rates over unnecessary SRC.
- Keep the M-Audio 44.1 reassertion as an FW410 startup quirk.
- Reacquire generation/node state after boot, reset and rate changes.
- Keep the reusable long-term architecture suitable for other FireWire audio devices.
