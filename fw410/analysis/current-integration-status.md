# FW410 current integration status

Last updated: 2026-08-18

This document is the current handoff for the **CoreAudio integration and next-work phase**. It supersedes the CoreAudio/AudioDriverKit transition and next-work sections of `current-development-status.md`; the older document remains the detailed canonical record for reverse-engineering, packet formation, AV/C/CMP, mixer/topology, boot and historical experiments.

## Current result

macfw now publishes **M-Audio FireWire 410** as a normal macOS CoreAudio output device through a dependency-free AudioServerPlugIn. The device is visible in Audio MIDI Setup and the macOS audio selector and exposes 44.1 and 48 kHz.

Hardware-backed normal application playback is confirmed at both native rates:

- **44.1 kHz:** clear stereo output on physical Analog Outputs 1/2, using `AmdtpPcmStream44100` and the required FW410 post-start AV/C 44.1 reassertion.
- **48 kHz:** clear stereo output on physical Analog Outputs 1/2 with no SRC after enlarging the TX scheduler from the old 128/64-cycle geometry to 640/320 cycles.

The proven architecture is:

```text
macOS application
    -> CoreAudio
    -> macfw AudioServerPlugIn
    -> WriteMix stereo Float32
    -> versioned shared-memory PCM ring
    -> user-space macfw transport
    -> native rate-specific AMDTP scheduler
    -> CMP / NuDCL / IOFireWireLib
    -> M-Audio FireWire 410
```

The HAL callback does not own FireWire. It only copies real-time PCM to shared memory. AV/C, CMP, FireWire ISO, rate control, startup quirks and eventual bus-reset recovery belong in the companion transport/service layer.

## Native 44.1 kHz

`tools/transport/halbridge44100` is hardware-confirmed clean.

Required startup behavior on the tested FW410:

1. set both signal-format directions to 44.1 kHz;
2. create duplex CMP connections;
3. start duplex ISO with valid native 44.1 NODATA/data scheduling;
4. while streaming is live, reassert 44.1 kHz on both AV/C directions;
5. continue normal native 44.1 playback.

The post-start rate reassertion is an FW410/M-Audio startup quirk and must stay in the device transport state machine rather than the generic AMDTP packet generator.

## Native 48 kHz

`tools/transport/halbridge48000` is hardware-confirmed with clear native 48 kHz output.

The HAL producer was measured at essentially exactly 48,000 frames/s, proving that the earlier poor 48 kHz audio was not caused by CoreAudio or sample-rate conversion. The decisive A/B result was transmit scheduling margin:

```text
old / broken under this HAL workload:
    TX ring:      128 cycles
    refill half:   64 cycles
    half window:   ~8 ms

new / clear:
    TX ring:      640 cycles
    refill half:  320 cycles
    half window:  ~40 ms
```

The committed 48 kHz path also uses:

- 16,384-frame internal PCM FIFO;
- drain-until-caught-up shared-memory consumption;
- user-interactive pthread QoS;
- a shorter CoreFoundation callback-service interval;
- query-before-set behavior so an FW410 already at 48 kHz is not disturbed by redundant no-op AV/C rate CONTROL;
- diagnostics for `lateCyclePolls` and scheduler-inserted silence.

### Residual load-sensitive dropouts

Small dropouts can still occur when the Mac is busy with unrelated desktop work. Current evidence does **not** identify these as PCM starvation or scheduler-generated silence:

- `framesSilenced` remains at zero when a dropout is heard;
- `lateCyclePolls` increases gradually during normal playback but does not show a distinct jump at dropout moments;
- the HAL producer remains at the expected 48 kHz cadence;
- enlarging the TX ring solved the major corruption, leaving only occasional load-sensitive glitches.

Treat this as unresolved user-space/FireWire servicing jitter below or beside the current high-level counters. Do not regress the proven 640/320 geometry while investigating it. A future transport-core refactor should separate lifecycle/logging from the latency-sensitive service loop and add finer timing instrumentation around cycle polling, TX-half refill/commit and callback wakeups.

## Rate-aware transport supervisor

`tools/transport/haltransport` is the first unified runtime entry point.

It deliberately does **not** duplicate or refactor the two hardware-proven packet engines yet. Instead it:

1. maps the HAL shared state;
2. reads the CoreAudio-selected sample rate;
3. starts the proven native `halbridge44100` or `halbridge48000` engine;
4. watches for HAL sample-rate changes;
5. cleanly stops the old engine and starts the matching native engine;
6. forwards SIGINT/SIGTERM shutdown through the child engine's guarded CMP/ISO cleanup.

This supervisor is an integration step, not the final service architecture. Once rate switching is hardware-validated through this entry point, common transport lifecycle code can be extracted into `fw410/lib` without rewriting the known-good AMDTP paths first.

## Immediate sequence

1. Hardware-validate `haltransport` at 44.1 and 48 kHz, including changing the HAL format while the supervisor is running.
2. Extract common 44.1/48 device/CMP/ISO lifecycle into a reusable transport core while preserving the proven rate-specific schedulers and the 44.1 startup quirk.
3. Move the latency-sensitive transport service loop away from logging/control work and add finer jitter timing diagnostics.
4. Expand the HAL playback presentation from temporary stereo to the real FW410 playback-channel topology.
5. Add FW410 -> macOS capture/input using the already-proven receive/AM824 code.
6. Make transport startup automatic so a user does not manually launch a companion binary.
7. Add bus-reset, generation/node change, disconnect/reconnect and rate-change recovery.
8. Add mixer/routing/headphone, S/PDIF and MIDI integration.
9. Finish packaging/release automation once the runtime is reproducibly installable.

## Bootloader / interface boot mode

Production startup must handle both FW410 personalities:

```text
FW Bootloader  model 0x00010058
FW 410         model 0x00010046
```

The repository already proves the bootloader -> operational transition from user space. When the companion transport becomes a persistent service, device startup must become a state machine:

1. discover the attached FireWire personality;
2. if it is `FW Bootloader`, execute the proven boot transition;
3. wait for FireWire re-enumeration;
4. reacquire the operational `FW 410` unit and fresh generation/node state;
5. configure the requested clock domain;
6. establish CMP/ISO and activate audio transport;
7. recover similarly after bus resets or reconnects.

Do not bolt boot handling into the HAL real-time callback. It belongs in the transport/device lifecycle service.

## Release direction

The agreed version format remains `x.yy.zzz`, with eventual tag-driven packages:

- `lite`: runtime/driver components only;
- `full`: runtime plus project tools;
- `source`: exact tagged repository `.tar.gz`.

See repository `RELEASES.md` for the release policy.
