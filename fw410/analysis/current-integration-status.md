# FW410 current integration status

Last updated: 2026-08-24

This document is the current handoff for the CoreAudio/HAL/runtime phase. Older reverse-engineering documents remain useful historical evidence, but this file defines the present integration state and immediate next work.

## Executive status

macfw publishes **M-Audio FireWire 410** as a normal macOS CoreAudio device through a dependency-free AudioServerPlugIn.

Hardware-confirmed today:

- the device appears in Audio MIDI Setup and the normal macOS audio selector;
- native 44.1 kHz and 48 kHz playback work;
- `haltransport` switches native playback engines when the CoreAudio rate changes;
- all eight analog outputs and both S/PDIF outputs were independently validated in Logic Pro;
- CoreAudio exposes four input channels: Analog In 1/2 and S/PDIF In L/R;
- native 48 kHz CoreAudio recording works in Logic Pro;
- the final 48 kHz capture receive path is steady-state clean under a controlled 1 kHz test;
- native 48 kHz full-duplex playback + capture is validated both directly and through `haltransport`;
- native 44.1 kHz full-duplex playback + capture is validated with the required post-start AV/C reassertion;
- `haltransport` has been hardware-validated switching repeatedly between 44.1 and 48 kHz while preserving playback and capture.

The current architecture is:

```text
macOS application
    -> CoreAudio
    -> macfw AudioServerPlugIn
       -> playback shared ring
       <- capture shared ring
    -> companion user-space transport
    -> AV/C / CMP / CIP / AMDTP / NuDCL / IOFireWireLib
    -> M-Audio FireWire 410
```

The HAL never owns FireWire. Real-time callbacks only move PCM through versioned lock-free shared memory. Device boot, rate control, CMP, ISO, AMDTP scheduling and recovery belong in the transport/service layer.

## Playback status

### CoreAudio topology

At 44.1/48 kHz CoreAudio presents:

```text
1  Analog Out 1
2  Analog Out 2
3  Analog Out 3
4  Analog Out 4
5  Analog Out 5
6  Analog Out 6
7  Analog Out 7
8  Analog Out 8
9  S/PDIF Out L
10 S/PDIF Out R
```

The raw FW410 playback order is unusual and is explicitly permuted by the transport. `analysis/stream-topology.md` is the mapping source of truth.

### Native 44.1 kHz

The 44.1 engine is hardware-confirmed and clean. The tested FW410 requires this startup sequence:

1. select 44.1 kHz in both AV/C signal-format directions;
2. establish duplex CMP/ISO;
3. start valid native 44.1 AMDTP traffic;
4. while streaming is live, reassert 44.1 on OUTPUT plug 0 and INPUT plug 0;
5. continue normal data-bearing scheduling.

This is an FW410/M-Audio startup quirk and must remain in the device lifecycle code.

### Native 48 kHz

The 48 kHz engine is hardware-confirmed and clear. The decisive fix for early corruption was increasing transmit scheduling margin from a 128-cycle ring / 64-cycle refill half to 640 / 320 cycles. The committed path also uses a 16,384-frame PCM FIFO, user-interactive transport QoS and late/silence diagnostics.

Some client/load-sensitive playback glitches have historically been easier to trigger with browser/YouTube playback than Logic Pro. They are not currently associated with scheduler-inserted silence and should be investigated separately from the now-solved capture path.

## Rate-aware full-duplex supervisor

`tools/transport/haltransport` is the current rate-aware full-duplex runtime supervisor. Both native engines are now validated: 44.1 kHz and 48 kHz each provide ten-channel playback and four-channel capture. The supervisor reads the HAL-selected sample rate, starts the matching native engine and restarts it when the rate changes. Repeated 44.1 <-> 48 kHz switching has been hardware-tested with playback and capture remaining functional after each transition.

Rate changes can cause FireWire generation/node transitions. A persistent runtime must explicitly reacquire a stable operational unit rather than assume old generation/node state remains valid.

## Capture topology

At 44.1/48 kHz the incoming FW410 AMDTP stream is DBS=5:

```text
raw position 1  S/PDIF In L
raw position 2  Analog In 1
raw position 3  S/PDIF In R
raw position 4  Analog In 2
raw position 5  MIDI
```

CoreAudio presents:

```text
Input 1  Analog In 1
Input 2  Analog In 2
Input 3  S/PDIF In L
Input 4  S/PDIF In R
```

The capture ABI is a separate four-channel Float32 shared ring defined in `hal/include/macfw_hal_capture_shm.h`.

## Validated 48 kHz CoreAudio capture

The complete capture path is now hardware-confirmed:

```text
FW410 input
 -> FireWire device-to-host ISO
 -> NuDCL receive ring
 -> 32-cycle (~4 ms) metadata publication groups
 -> terminal-slot-confirmed completed group
 -> AMDTP/CIP/DBC validation
 -> AM824 MBLA-24 decode
 -> four-channel Float32 shared capture ring
 -> AudioServerPlugIn ReadInput
 -> Logic Pro
```

### Shared-memory lifecycle

The capture object is persistent (`/macfw_fw410_capture_v2`). The HAL maps it outside the real-time callback, and the producer reinitializes the same object in place. This avoids splitting a long-lived `coreaudiod` mapping from a newly recreated producer object.

The producer keeps the ring inactive until CoreAudio has started issuing `ReadInput` and 4,096 frames (~85 ms at 48 kHz) have accumulated. It then positions the read cursor at that cushion and enables live capture.

### FW410 duplex requirement

The FW410 only remains in sample-bearing capture state while valid host-to-device AMDTP also flows. The standalone `capturebridge48000` therefore runs the normal 48 kHz playback scheduler with an empty 10-channel FIFO, producing correctly timed digital silence as a keepalive.

That silence keepalive remains useful only in the standalone capture test. The production 48 kHz engine now replaces it with real ten-channel CoreAudio playback while preserving the same duplex scheduling behavior.

### Receive-publication breakthrough

The early receive ring published the whole 256-slot ring at once (~32 ms). Capture was recognizable but badly corrupted because userspace could observe metadata/payload slots while earlier DMA locations were already being reused.

Publishing 32-cycle groups (~4 ms) made audio almost clean, but a global scan across all changed slots could still combine independently published groups.

The final validated rule is stricter:

- every group contains 32 receive DCL slots;
- the terminal receive DCL publishes the group's metadata update list;
- userspace treats a changed terminal-slot completion token as proof that the group completed: `(timestamp, isoHeader)` at 48 kHz and timestamp-only at 44.1 kHz;
- userspace snapshots only those 32 slots;
- DBC continuity is validated/ordered only inside that completed group.

This keeps the NuDCL program receive-only and removes the mixed-generation snapshots responsible for the residual cracks.

Representative final run:

```text
capture frames=940064 (delta 96000)
active=1 queued=3968 drops=0
malformed=0 invalid=0
chunks=4981
dbc-gap=0 ts-back=4 reorder=0 stale=0
```

The chunk counter increased by ~500 every two-second report, matching 250 completed 32-cycle groups per second.

### Controlled quality evidence

A 1 kHz, 1.0 V, 60% duty-cycle source was recorded in Logic Pro at 48 kHz. Three development recordings showed the progression:

| Recording | State | Discontinuity clusters | Approx. rate |
|---|---|---:|---:|
| test 17 | early/full-ring receive | 434 | 5.75/sec |
| test 19 | 32-cycle publication + global DBC ordering | 70 | 2.23/sec |
| test 20 | completed-group consumption | 1 startup event | 0.03/sec |

After the startup region of test 20, no significant steady-state discontinuities were detected and no dropouts were subjectively audible.

Detailed design and evidence: `analysis/capture-pipeline.md`.

## HAL reload caveat

During development, rebuilding/installing the AudioServerPlugIn and restarting `coreaudiod` did not always guarantee that the newly installed HAL binary was active. A full reboot was sometimes required before behavior matched the newly built code.

This is a separate lifecycle/debugging issue, not a capture-transport defect. Future instrumentation should expose an explicit runtime HAL build/version identifier so tests can prove which binary `coreaudiod` is executing.

## Bootloader / interface boot mode

Production startup must support both personalities:

```text
FW Bootloader  model 0x00010058
FW 410         model 0x00010046
```

The repository already proves the guarded bootloader -> operational transition. A persistent service must discover the current personality, boot if needed, wait for re-enumeration, reacquire generation/node state, set the requested clock domain and then establish CMP/ISO.

Do not put this lifecycle work in a HAL real-time callback.

## Validated 48 kHz full duplex

The real 10-channel playback engine and the completed-chunk four-channel capture engine now run together in `halbridge48000`.

The first combined version regressed capture quality even though DBC/order/drop diagnostics stayed clean. The cause was service ordering: playback SHM draining/mapping ran before receive consumption. TX has substantial prebuilt scheduling margin, while RX slots are continuously reused by DMA.

The validated loop therefore prioritizes capture:

```text
capture service
 -> playback SHM/PCM work
 -> TX scheduler service
 -> capture service again
```

After this change, simultaneous playback, live monitoring and recording were reported clean. Representative full-duplex runs held exact 48 kHz cadence with zero capture drops, malformed packets, invalid labels, DBC gaps, reorders and stale packets. The same behavior was confirmed when the engine was started through `haltransport`.

Live-monitor latency remains intentionally high because the current capture prefill is 4,096 frames (~85 ms). Latency reduction is deferred until transport correctness across both rates is complete.

## Validated 44.1 kHz full duplex

The four-channel completed-chunk capture model is now integrated into `halbridge44100` alongside the already-proven ten-channel playback engine.

The first full-duplex 44.1 kHz receive implementation reused the 48 kHz terminal-slot `(timestamp, isoHeader)` completion signature. It produced capture too quickly, the shared capture queue grew continuously and eventually dropped frames, and recording/monitoring were audibly broken. At 44.1 kHz the alternating blocking/NODATA AMDTP pattern makes the ISO header unsuitable as part of the completed-chunk generation token.

The validated rate-specific completion rule is:

```text
48 kHz: terminal-slot timestamp + isoHeader
44.1 kHz: terminal-slot timestamp only
```

With timestamp-only completion at 44.1 kHz, the receive path returned to correct native cadence. Representative hardware runs showed capture deltas around 88,200 frames per two-second report, a stable ~4,000-frame queue, and zero `in-drops`, malformed packets, invalid labels, DBC gaps, reorders and stale groups. Recording and live monitoring were reported clear.

The existing 44.1 startup sequence remains mandatory: select 44.1 by AV/C, establish duplex ISO, start native traffic, then reassert 44.1 on OUTPUT and INPUT plug 0 while streaming is live.

## Both native rates validated under haltransport

The production supervisor has now been exercised while alternating the CoreAudio format between 44.1 and 48 kHz. Both playback and capture survive the transitions and resume at the correct native cadence. This validates the current stop/restart, rate-control, re-enumeration/reacquisition and 44.1 post-start reassert path as an end-to-end runtime sequence.

Current functional matrix:

| Capability | 44.1 kHz | 48 kHz |
|---|---|---|
| 10-channel playback | validated | validated |
| 4-channel capture | validated | validated |
| simultaneous full duplex | validated | validated |
| `haltransport` launch | validated | validated |
| runtime rate switching | validated | validated |

## Immediate sequence

1. Extract common 44.1/48 device, CMP, ISO and full-duplex lifecycle into a reusable transport core without changing the now-validated packet paths.
2. Add automatic startup/bootloader handling and robust bus-reset, generation-change, disconnect/reconnect and rate-change recovery.
3. Add a runtime HAL build identifier to eliminate stale-load ambiguity during development.
4. Tune capture prefill / monitoring latency now that transport correctness is stable at both native rates.
5. Return to mixer/routing/headphone, S/PDIF and MIDI integration.
6. Finish packaging/release automation when the runtime is reproducibly installable.

## Quick handoff

> Native 44.1 and 48 kHz full duplex are now both hardware-validated through the normal `haltransport` path: ten-channel playback and four-channel capture run simultaneously at either native rate, and repeated 44.1 <-> 48 kHz switching works. The 44.1 receive path requires a timestamp-only terminal-slot completion token, while 48 kHz uses `(timestamp, isoHeader)`. The next task is to extract the duplicated rate-specific device/CMP/ISO/full-duplex lifecycle into a reusable transport core without disturbing these validated packet paths.
