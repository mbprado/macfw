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

`tools/transport/halbridge48000` is the current native 48 kHz integration test.

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

The current committed 48 kHz test also uses:

- 16,384-frame internal PCM FIFO;
- drain-until-caught-up shared-memory consumption;
- user-interactive pthread QoS as the current scheduler-jitter experiment;
- a shorter CoreFoundation callback-service interval;
- diagnostics for `lateCyclePolls` and scheduler-inserted silence;
- query-before-set behavior so an FW410 already at 48 kHz is not disturbed by redundant no-op AV/C rate CONTROL.

Small residual dropouts seen before the QoS change correlated with unrelated desktop activity and temporary shared-memory backlog. Treat this as user-space scheduling jitter unless new evidence indicates otherwise.

## Immediate sequence

1. Hardware-validate the committed 48 kHz QoS/jitter changes under desktop load.
2. Replace `halbridge44100` and `halbridge48000` with one rate-aware transport service that reads the HAL-selected rate and selects the native 44.1 or 48 scheduler automatically.
3. Expand the HAL playback presentation from temporary stereo to the real FW410 playback-channel topology.
4. Add FW410 -> macOS capture/input using the already-proven receive/AM824 code.
5. Make transport startup automatic so a user does not manually launch a companion binary.
6. Add bus-reset, generation/node change, disconnect/reconnect and rate-change recovery.
7. Add mixer/routing/headphone, S/PDIF and MIDI integration.
8. Finish packaging/release automation once the runtime is reproducibly installable.

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
