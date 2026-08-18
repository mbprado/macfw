# FW410 current integration status

Last updated: 2026-08-18

This document is the current handoff for the **CoreAudio integration and next-work phase**. It supersedes the CoreAudio/AudioDriverKit transition and next-work sections of `current-development-status.md`; the older document remains the detailed canonical record for reverse-engineering, packet formation, AV/C/CMP, mixer/topology, boot and historical experiments.

## Current result

macfw publishes **M-Audio FireWire 410** as a normal macOS CoreAudio output device through a dependency-free AudioServerPlugIn. The device is visible in Audio MIDI Setup and the macOS audio selector and exposes native 44.1 and 48 kHz operation.

Hardware-backed normal application playback is confirmed at both native rates:

- **44.1 kHz:** clear output using `AmdtpPcmStream44100` and the required FW410 post-start AV/C 44.1 reassertion.
- **48 kHz:** clear output with no SRC after enlarging the TX scheduler from the old 128/64-cycle geometry to 640/320 cycles.
- **rate switching:** `haltransport` has been hardware-confirmed switching 44.1 -> 48 -> 44.1 while running, selecting the matching native transport automatically.

The proven architecture is:

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

The HAL callback does not own FireWire. It only copies real-time PCM to shared memory. AV/C, CMP, FireWire ISO, rate control, startup quirks and eventual bus-reset recovery belong in the companion transport/service layer.

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

The HAL/shared-memory ABI is now version 3 and carries 10 interleaved Float32 output channels in this physical order. Both native transports explicitly permute that order into the FW410 AMDTP positions above.

## Native 44.1 kHz

`tools/transport/halbridge44100` is hardware-confirmed clean.

Required startup behavior on the tested FW410:

1. set both signal-format directions to 44.1 kHz;
2. create duplex CMP connections;
3. start duplex ISO with valid native 44.1 NODATA/data scheduling;
4. while streaming is live, reassert 44.1 kHz on both AV/C directions;
5. continue normal native 44.1 playback.

The post-start rate reassertion is an FW410/M-Audio startup quirk and must stay in the device transport state machine rather than the generic AMDTP packet generator.

The 44.1 status output now reports the same `drops`, `late` and `silence` counters as the 48 kHz path.

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
- enlarging the TX ring solved the major corruption, leaving only occasional load-sensitive glitches.

Treat this as unresolved user-space/FireWire servicing jitter below or beside the current high-level counters. Do not regress the proven 640/320 geometry while investigating it.

## Rate-aware transport supervisor

`tools/transport/haltransport` is hardware-confirmed as the unified runtime entry point for 44.1/48 playback.

It maps HAL shared state, reads the selected CoreAudio sample rate, starts the proven native rate-specific engine, watches for rate changes, and stops/restarts the matching engine through guarded CMP/ISO cleanup.

A 48 -> 44.1 transition exposed a useful lifecycle edge case: immediately after a rate change/re-enumeration, a first child attempt can temporarily observe a stale/transitional FireWire state such as node `0x0` and unknown AV/C rate. The supervisor retry then reacquires the normal node and succeeds. The eventual persistent service should explicitly wait for/reacquire a stable operational unit rather than relying on a failed child/retry cycle.

## Immediate sequence

1. Hardware-validate the new 10-channel HAL/shared-memory/transport path at 44.1 and 48 kHz.
2. Verify multichannel routing from CoreAudio applications/Audio MIDI Setup, especially Analog 1-8 and S/PDIF L/R ordering.
3. Extract common 44.1/48 device/CMP/ISO lifecycle into a reusable transport core while preserving the proven rate-specific schedulers and 44.1 startup quirk.
4. Move latency-sensitive transport servicing away from logging/control work and add finer jitter timing diagnostics.
5. Add FW410 -> macOS capture/input using the already-proven receive/AM824 code.
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
