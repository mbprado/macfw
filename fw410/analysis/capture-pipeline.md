# FW410 CoreAudio capture pipeline

Last updated: 2026-08-19

This document is the source of truth for the hardware-validated native 48 kHz capture path from the M-Audio FireWire 410 into normal macOS CoreAudio applications.

## Validated result

Native 48 kHz capture is working end to end through the macfw AudioServerPlugIn and was validated in Logic Pro with a controlled 1 kHz source.

```text
FW410 Analog/S/PDIF input
    -> device-to-host FireWire ISO
    -> NuDCL receive ring
    -> completed 32-cycle publication groups
    -> AMDTP/CIP validation and DBC continuity
    -> AM824 MBLA-24 decode
    -> four-channel Float32 shared capture ring
    -> AudioServerPlugIn ReadInput
    -> CoreAudio application / Logic Pro
```

CoreAudio-facing input order:

1. Analog In 1
2. Analog In 2
3. S/PDIF In L
4. S/PDIF In R

Raw FW410 AMDTP order at 44.1/48 kHz:

1. S/PDIF In L
2. Analog In 1
3. S/PDIF In R
4. Analog In 2
5. MIDI

The transport explicitly permutes the four audio positions into the CoreAudio-facing order.

## Shared-memory capture ABI

The capture path uses persistent POSIX shared memory:

```text
/macfw_fw410_capture_v2
```

The ring is defined by `hal/include/macfw_hal_capture_shm.h` and carries:

- four interleaved Float32 channels;
- 32,768-frame capacity;
- monotonic write/read frame counters;
- producer state and sample rate;
- packet/decode diagnostics;
- HAL `ReadInput` consumption diagnostics.

The HAL maps the object outside the real-time callback. The producer reuses and reinitializes that same persistent object in place. `ReadInput` performs lock-free reads and zero-fills only while capture is unavailable or genuinely underruns.

## Duplex requirement

The FW410 does not remain in sample-bearing capture state from a receive connection alone. Valid host-to-device AMDTP must continue flowing.

`capturebridge48000` therefore establishes both CMP/ISO directions and services the proven 48 kHz playback scheduler with an empty 10-channel PCM FIFO. This produces correctly timed digital silence as a capture keepalive.

The production full-duplex runtime should replace that silence with the real 10-channel CoreAudio playback ring while preserving the same duplex startup and servicing behavior.

## Capture prefill

CoreAudio can begin issuing `ReadInput` before the FireWire receive path reaches steady state. The bridge therefore:

1. initializes the capture ring inactive;
2. waits until HAL `ReadInput` activity is observed;
3. accumulates 4,096 frames (~85 ms at 48 kHz);
4. positions the read cursor at that controlled live-edge cushion;
5. publishes `active=1`.

This eliminates startup starvation without turning the steady-state path into a high-latency queue.

## NuDCL receive publication problem

The first working receive implementation published metadata for the full 256-slot ring at the end of every revolution. At the 8 kHz FireWire cycle rate that exposed data in roughly 32 ms bursts.

The captured signal was recognizable but badly corrupted. Shared-memory buffering and CoreAudio consumption were healthy; the corruption originated earlier because userspace could scan slots while early DMA payload locations were already being reused by the following ring revolution.

Reducing publication to 32-cycle groups (~4 ms) made the recording almost clean. However, globally scanning every changed slot across all 256 descriptors could still combine independently published groups and create occasional temporal discontinuities.

## Final completed-group rule

The final validated receive rule is:

- each publication group contains 32 receive DCL slots;
- the terminal receive DCL publishes that group's metadata update list via `SetDCLUpdateList`;
- userspace treats a changed terminal-slot `(timestamp, isoHeader)` signature as the completed-group token;
- only then are the exact 32 slots in that group snapshotted;
- AMDTP DBC continuity is validated and ordering is applied only inside that completed group.

This retains a receive-only NuDCL program. A speculative mixed send/receive completion-marker experiment was explicitly rejected before hardware testing.

Representative final run:

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

The expected publication cadence is:

```text
8000 FireWire cycles/sec / 32 cycles/group = 250 groups/sec
```

Therefore the chunk counter should increase by about 500 every two-second reporting interval, which is what the validated run showed.

## Controlled quality validation

The final validation source was a 1 kHz, 1.0 V, 60% duty-cycle signal recorded as mono 48 kHz audio in Logic Pro.

The progression was measurable:

| Recording | Receive behavior | Detected discontinuity clusters | Approximate rate |
|---|---|---:|---:|
| test 17 | early/full-ring receive path | 434 | 5.75/sec |
| test 19 | 32-cycle publication plus global DBC ordering | 70 | 2.23/sec |
| test 20 | terminal-slot completed groups | 1 startup event | 0.03/sec |

After the initial startup region in test 20, no significant periodic discontinuities were detected for the remainder of the recording. Subjectively, no further dropouts were audible.

Short comparison excerpts are stored under `pictures/audio/` rather than committing the full development recordings:

- `capture-test19-before.wav`
- `capture-test20-after.wav`

## Diagnostics

`capturebridge48000` reports:

- `chunks`: completed 32-slot groups consumed;
- `dbc-gap`: accepted packet sequence discontinuities;
- `ts-back`: non-forward NuDCL timestamp observations;
- `reorder`: packets emitted in a different order inside a completed group;
- `stale`: packets rejected as behind the live DBC sequence;
- `drops`: frames rejected because the shared capture ring was full;
- `malformed`: invalid CIP/formation packets;
- `invalid`: invalid AM824 MBLA labels;
- `hal-read`: HAL `ReadInput` calls;
- `tx-late` and `tx-silence`: playback-keepalive scheduler diagnostics.

In the final controlled run, `dbc-gap`, `reorder`, `stale`, `drops`, `malformed`, and `invalid` remained zero. Small `ts-back` increments did not correlate with audible or sample-level discontinuities and are treated as a timestamp diagnostic edge case rather than evidence of reordered PCM.

## What is solved vs pending

Solved at 48 kHz:

- FireWire capture and MBLA decode;
- physical channel mapping;
- persistent capture SHM lifecycle;
- CoreAudio `ReadInput` delivery;
- startup prefill;
- receive publication consistency;
- completed-group consumption;
- controlled steady-state recording quality.

Still pending:

- merge capture with real playback in one full-duplex runtime;
- native 44.1 capture integration/validation;
- automatic boot/startup;
- bus-reset and reconnect recovery;
- persistent-service lifecycle;
- mixer/control/MIDI integration.

The next primary implementation step is full-duplex 48 kHz operation in the normal transport: consume the 10-channel playback shared ring while simultaneously feeding the proven four-channel capture shared ring.
