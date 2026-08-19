# M-Audio FireWire 410 — Modern macOS Driver / DevKit

Reverse-engineering and user-space driver/development-kit project for the M-Audio FireWire 410.

## Current result

On the tested Intel Mac, macfw now publishes **M-Audio FireWire 410** as a normal CoreAudio device through an AudioServerPlugIn and connects that device to real FireWire hardware from user space.

Hardware-validated today:

- guarded bootloader-to-operational transition;
- asynchronous FireWire and FCP/AV/C control;
- IRM/CMP duplex connection management;
- user-space NuDCL receive and transmit;
- native 44.1 and 48 kHz playback;
- all 10 playback channels validated in Logic Pro: Analog 1-8 and S/PDIF L/R;
- native 48 kHz four-channel CoreAudio capture validated in Logic Pro;
- capture inputs exposed as Analog 1/2 and S/PDIF L/R;
- controlled 1 kHz capture with no significant steady-state discontinuities after completed-chunk receive handling;
- rate-aware 44.1/48 playback transport supervision;
- AV/C sample-rate control and FW410-specific native-44.1 startup handling.

The present architecture is:

```text
macOS application
    -> CoreAudio
    -> macfw AudioServerPlugIn
    -> versioned shared-memory playback/capture rings
    -> user-space macfw transport
    -> AV/C / CMP / NuDCL / AMDTP
    -> M-Audio FireWire 410
```

The HAL callback never owns FireWire. It only performs real-time-safe shared-memory audio transfer. Device boot, rate control, CMP, ISO scheduling, startup quirks and eventual bus-reset recovery remain in the companion transport/service layer.

## Primary goal

The goal is not to port the original M-Audio kernel extension. The goal is a reusable modern macOS user-space audio stack for legacy FireWire interfaces, with the FW410 as the first proven backend.

Only FW410-specific layers should contain M-Audio assumptions.

## Hardware

- Device: M-Audio FireWire 410
- Interface: IEEE 1394 / FireWire
- Family: BridgeCo/BeBoB FireWire audio
- Tested platform: Intel macOS with Apple `IOFireWireLib`

Confirmed identity transition:

```text
before: FW Bootloader / model 0x00010058
after:  FW 410        / model 0x00010046
```

## Playback

### CoreAudio presentation

At 44.1/48 kHz, CoreAudio exposes:

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

The transport permutes this physical order into the FW410's raw AMDTP stream positions. Every output was independently validated in Logic Pro.

### Native 44.1 kHz

The FW410 requires an M-Audio-specific startup sequence:

1. select 44.1 kHz in both AV/C signal-format directions;
2. establish duplex CMP/ISO;
3. start valid native 44.1 AMDTP;
4. while streaming is live, reassert 44.1 kHz on both OUTPUT plug 0 and INPUT plug 0;
5. continue the normal data-bearing schedule.

The reassertion is a device startup quirk, not generic AMDTP behavior.

### Native 48 kHz

Clear 48 kHz playback required increasing transmit scheduling margin from the early 128-cycle/64-cycle-half geometry to 640 cycles with 320-cycle refill halves. The committed path also uses a 16,384-frame PCM FIFO and a continuously serviced native scheduler.

## Capture

Native 48 kHz CoreAudio capture is hardware-validated.

CoreAudio presents:

1. Analog In 1
2. Analog In 2
3. S/PDIF In L
4. S/PDIF In R

The raw FW410 capture formation is `DBS=5`: four PCM positions plus MIDI. The transport decodes AM824 MBLA-24 samples and permutes the raw order into the CoreAudio-facing order above.

The decisive receive fix was to stop scanning arbitrary changed slots across the full cyclic receive ring. Metadata is published in 32-cycle groups, and userspace consumes a group only after its terminal receive slot proves that the group was completed. DBC continuity is then validated within that completed group.

A controlled 1 kHz validation showed the progression:

| Test | Receive behavior | Detected discontinuity clusters |
|---|---|---:|
| 17 | early/full-ring path | 434 |
| 19 | 32-cycle publication and global ordering | 70 |
| 20 | terminal-slot completed chunks | 1 startup event |

After startup in test 20, no significant periodic discontinuities were detected and no further dropouts were audible.

See [`analysis/capture-pipeline.md`](analysis/capture-pipeline.md) for the complete capture design, diagnostics and validation evidence.

## Stream topology

The channel mapping source of truth is [`analysis/stream-topology.md`](analysis/stream-topology.md).

At 44.1/48 kHz:

- host playback: 10 PCM + 1 MIDI;
- host capture: 4 PCM + 1 MIDI.

Higher-rate formations have been probed but are not yet equivalent to the validated native 44.1/48 paths.

## Development milestones

### M1 — User-space FireWire proof

- [x] Detect and boot the FW410
- [x] Read configuration ROM and BeBoB information
- [x] Async read/write and FCP/AV/C
- [x] Allocate isochronous resources
- [x] Establish duplex transport
- [ ] Repeat the complete path on newer Intel macOS

### M2 — FireWire transport

- [x] Node discovery
- [x] FCP transport
- [x] IRM allocation and CMP lifecycle
- [x] NuDCL RX/TX
- [x] CIP/AM824 parsing
- [x] Reusable RX/TX ring infrastructure
- [x] Native 44.1 and 48 kHz playback
- [x] Stable 48 kHz completed-chunk capture
- [ ] Bus-reset-safe persistent stream restart

### M3 — FW410 / BeBoB protocol

- [x] Device/firmware identification and boot
- [x] Stream formation and channel mapping
- [x] Duplex packet-flow requirement
- [x] 44.1/48 sample-rate control
- [x] Native-44.1 startup quirk
- [x] Mixer/selector topology and headphone selector candidate
- [ ] Complete clock/control mapping
- [ ] MIDI byte transport validation

### M4 — Audio DevKit

- [x] Playback PCM encoder
- [x] Capture PCM decoder
- [x] PCM rings and native schedulers
- [x] Versioned shared playback/capture ABIs
- [x] Completed-group receive consumption
- [ ] Unified persistent transport lifecycle
- [ ] Generic controls and MIDI abstractions

### M5 — CoreAudio integration

- [x] CoreAudio device visible in Audio MIDI Setup
- [x] Native 44.1/48 playback exposed to applications
- [x] Ten playback channels exposed and validated
- [x] Four 48 kHz capture channels exposed and validated
- [x] Shared-memory HAL/transport bridge
- [ ] Merge proven capture with real playback in the normal full-duplex transport
- [ ] Native 44.1 capture validation
- [ ] Automatic companion-service startup and recovery

### M6 — Complete FW410 runtime

- [ ] Persistent service and packaging
- [ ] Bus-reset/disconnect recovery
- [ ] Mixer/control panel and routing
- [ ] Headphone controls
- [ ] MIDI
- [ ] Higher sample rates

## Repository guide

- `tools/device/` — discovery, ROM and guarded boot tools
- `tools/control/` — AV/C, topology, rate and mixer probes
- `tools/transport/` — playback, capture and CoreAudio transport experiments
- `lib/` — reusable FireWire/AMDTP components
- `hal/` — AudioServerPlugIn and shared-memory ABIs
- `analysis/` — current handoffs and reverse-engineering findings
- `pictures/` — milestone images and short validation media

Current handoffs:

- [`analysis/current-integration-status.md`](analysis/current-integration-status.md)
- [`analysis/capture-pipeline.md`](analysis/capture-pipeline.md)
- [`analysis/current-development-status.md`](analysis/current-development-status.md)
- [`HISTORY.md`](HISTORY.md)

## Immediate next work

1. Merge the validated 48 kHz capture engine into the normal rate-aware transport.
2. Replace capture's digital-silence playback keepalive with real ten-channel CoreAudio playback for full duplex.
3. Validate simultaneous playback and recording under sustained load.
4. Add native 44.1 capture while preserving the FW410 startup reassertion.
5. Add automatic boot/startup, bus-reset and reconnect recovery.
6. Return to mixer, routing, headphone and MIDI integration.

## Release policy

Release/version/package policy is documented in [`../RELEASES.md`](../RELEASES.md). The planned packages are `lite`, `full` and `source` from the same version tag.

## Disclaimer

This is an independent reverse-engineering and compatibility project. It is not affiliated with or supported by M-Audio, Avid, Apple or any hardware manufacturer. Vendor drivers, firmware and proprietary material remain subject to their respective licenses and copyrights.
