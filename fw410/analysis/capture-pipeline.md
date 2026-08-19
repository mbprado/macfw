# FW410 CoreAudio capture pipeline

Last updated: 2026-08-19

This document is the source of truth for the hardware-validated 48 kHz capture path from the M-Audio FireWire 410 into normal macOS CoreAudio applications.

## Validated result

Native 48 kHz capture is working end to end through the macfw AudioServerPlugIn and was validated in Logic Pro with a controlled 1 kHz source.

```text
FW410 Analog/S/PDIF input
    -> device-to-host FireWire ISO
    -> NuDCL receive ring
    -> completed 32-cycle publication chunks
    -> AMDTP/CIP validation and DBC continuity
    -> AM824 MBLA-24 decode
    -> 4-channel Float32 shared capture ring
    -> AudioServerPlugIn ReadInput
    -> CoreAudio application / Logic Pro
```

The CoreAudio-facing input order is:

1. Analog In 1
2. Analog In 2
3. S/PDIF In L
4. S/PDIF In R

The raw FW410 AMDTP stream order at 44.1/48 kHz is:

1. S/PDIF In L
2. Analog In 1
3. S/PDIF In R
4. Analog In 2
5. MIDI

The transport explicitly permutes the four audio positions into the CoreAudio-facing order.

## Shared-memory capture ABI

The capture path uses POSIX shared memory named:

```text
/macfw_fw410_capture_v2
```

The ring is defined by `hal/include/macfw_hal_capture_shm.h` and contains:

- four interleaved Float32 channels;
- 32,768 frame capacity;
- monotonic write/read frame counters;
- producer state and sample rate;
- packet/decode diagnostics;
- HAL `ReadInput` consumption diagnostics.

The HAL maps the ring outside the real-time callback. `ReadInput` performs only lock-free reads and zero-fills when the producer is unavailable or the ring genuinely underruns.

## Duplex requirement

The FW410 does not remain in its sample-bearing capture state from a receive connection alone. Valid host-to-device AMDTP must continue flowing.

`capturebridge48000` therefore establishes both CMP/ISO directions and services the proven 48 kHz playback scheduler with an empty ten-channel PCM FIFO. The resulting correctly timed digital silence is a keepalive for capture, not an audio source presented to the user.

The production full-duplex transport should replace this silence keepalive with the real ten-channel CoreAudio playback ring while preserving the same duplex startup and servicing behavior.

## Capture prefill

CoreAudio begins issuing `ReadInput` before the FireWire receive path has necessarily reached steady state. The bridge therefore:

1. initializes the capture ring inactive;
2. waits until HAL `ReadInput` activity is observed;
3. accumulates 4,096 frames, approximately 85 ms at 48 kHz;
4. places the read cursor at that controlled live-edge cushion;
5. publishes `active=1`.

This eliminates startup starvation without increasing steady-state transport buffering indefinitely.

## NuDCL receive publication problem

The first working receive implementation published metadata for the full 256-slot ring at the end of every revolution. At the 8 kHz FireWire cycle rate this exposed data in roughly 32 ms bursts.

The capture was recognizable but badly corrupted because userspace could scan slots while early DMA payload locations were already being reused by the following revolution. Shared-memory buffering and CoreAudio consumption were healthy; the corruption originated before the decoded PCM ring.

Reducing publication to 32-cycle groups, approximately 4 ms, made the recording almost clean. However, scanning every changed slot across all 256 descriptors could still combine independently published groups and create occasional temporal discontinuities.

## Completed-chunk consumption

The final validated receive rule is:

- each group contains 32 receive DCL slots;
- metadata for that group is published by the group terminal receive DCL using `SetDCLUpdateList`;
- userspace treats a changed terminal-slot `(timestamp, isoHeader)` signature as the completed-group token;
- only then are the exact 32 slots in that group snapshotted;
- AMDTP DBC continuity is validated and ordering is applied only inside that completed group.

This retains a receive-only NuDCL program and avoids speculative mixed send/receive completion markers.

Representative clean-run status:

```text
capture frames=940064 (delta 96000)
active=1
queued=3968
drops=0
malformed=0
invalid=0
chunks=4981
dbc-gap=0
ts-back=4
reorder=0
stale=0
```

Expected chunk cadence is approximately 500 groups per second:

```text
8000 FireWire cycles/sec / 32 cycles per group = 250 groups/sec
```

Because the status counter represents completed publication observations from the cyclic ring in the current implementation, the observed run increased by approximately 500 per two-second reporting interval, matching 250 groups/sec.

## Controlled quality validation

The final validation source was a 1 kHz, 1.0 V, 60% duty-cycle signal recorded as mono 48 kHz audio in Logic Pro.

Three development recordings showed the effect of the receive fixes:

| Recording | Receive behavior | Detected discontinuity clusters | Approximate rate |
|---|---|---:|---:|
| test 17 | full-ring/early capture path | 434 | 5.75/sec |
| test 19 | 32-cycle publication plus global DBC ordering | 70 | 2.23/sec |
| test 20 | terminal-slot completed chunks | 1 startup event | 0.03/sec |

After the initial startup region in test 20, no significant periodic discontinuities were detected for the remainder of the recording. Subjectively, no further dropouts were audible.

Short comparison excerpts are stored under `pictures/audio/`:

- `capture-test19-before.wav`
- `capture-test20-after.wav`

These are four-second mono 48 kHz PCM excerpts, not the full development recordings.

## Diagnostics

`capturebridge48000` reports:

- `chunks`: completed 32-slot groups consumed;
- `dbc-gap`: accepted packet sequence discontinuities;
- `ts-back`: non-forward NuDCL timestamp observations;
- `reorder`: packets emitted in a different order within a completed group;
- `stale`: packets rejected as behind the live DBC sequence;
- `drops`: frames rejected because the shared capture ring was full;
- `malformed`: invalid CIP/formation packets;
- `invalid`: invalid AM824 MBLA labels;
- `hal-read`: HAL `ReadInput` calls;
- `tx-late` and `tx-silence`: playback-keepalive scheduler diagnostics.

In the final controlled run, `dbc-gap`, `reorder`, `stale`, `drops`, `malformed`, and `invalid` remained zero. Small `ts-back` increments did not correlate with audible or sample-level discontinuities and are currently treated as a diagnostic timestamp edge case rather than an audio-order failure.

## Current limitations and next integration step

- This validated capture engine is currently exercised through standalone `capturebridge48000`.
- Native 44.1 kHz capture still needs equivalent integration and validation.
- Capture and real playback must be merged into the rate-aware `haltransport` runtime.
- Automatic startup, bootloader handling, FireWire bus-reset recovery, reconnect recovery, and persistent-service lifecycle remain pending.

The next primary implementation step is full-duplex 48 kHz operation in the normal transport: consume the ten-channel playback shared ring while simultaneously feeding the proven four-channel capture shared ring.
