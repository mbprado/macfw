# M-Audio FireWire 410 — Modern macOS Driver / DevKit

Reverse-engineering and user-space driver/DevKit project for the M-Audio FireWire 410 (FW410).

## Goal

The goal is not to port the original M-Audio kext. The project is building a modern macOS audio stack that keeps the CoreAudio-facing driver separate from a user-space FireWire transport service and can eventually be reused for other legacy FireWire audio interfaces.

```text
CoreAudio application
        |
macfw AudioServerPlugIn
        |
versioned shared-memory audio rings
        |
macfw transport service
        |
AV/C / CMP / CIP / AMDTP / NuDCL / IOFireWireLib
        |
M-Audio FireWire 410
```

## Hardware-validated status

On the tested Intel Mac, ordinary user-space processes using Apple's `IOFireWireLib` can:

- discover and boot the FW410 from its bootloader personality;
- perform asynchronous FireWire reads/writes and FCP/AV/C transactions;
- discover plug topology, supported rates and stream formations;
- allocate IRM resources and establish/restore both CMP directions;
- transmit and receive NuDCL isochronous programs entirely from user space;
- decode FW410 AM824 capture to signed 24-bit PCM;
- transmit correctly timed 48 kHz and native 44.1 kHz AM824 playback;
- read and change the FW410 sample rate through AV/C;
- expose the interface as a normal macOS CoreAudio device through an AudioServerPlugIn.

CoreAudio integration is hardware-confirmed for:

- native 44.1 kHz playback;
- native 48 kHz playback;
- all 10 playback channels: Analog Out 1-8 and S/PDIF L/R;
- native 48 kHz four-channel capture: Analog In 1/2 and S/PDIF In L/R;
- application recording in Logic Pro;
- runtime 44.1 <-> 48 kHz playback switching through `haltransport`.

The 48 kHz capture path was validated with a controlled 1 kHz source. After the final receive-side fix, a representative run held exact 96,000-frame two-second cadence with zero shared-ring drops, zero malformed packets, zero invalid MBLA labels, zero DBC gaps, zero packet reorders and zero stale packets. The final controlled recording contained no significant steady-state discontinuities.

Detailed capture design and evidence: [`analysis/capture-pipeline.md`](analysis/capture-pipeline.md).

## Boot identity

```text
before: FW Bootloader / model 0x00010058
after:  FW 410        / model 0x00010046
```

The eventual persistent transport service must handle this personality transition automatically and reacquire fresh FireWire generation/node state after re-enumeration.

## Playback

### Native 44.1 kHz

Native 44.1 playback is hardware-confirmed. The FW410 requires an M-Audio-specific startup ritual:

1. select 44100 in both AV/C signal-format directions;
2. establish duplex CMP/ISO;
3. begin valid native 44.1 AMDTP traffic;
4. while duplex streaming is live, reassert 44100 on OUTPUT plug 0 and INPUT plug 0;
5. continue normal data-bearing scheduling.

This quirk belongs in the FW410 transport state machine, not in the generic AMDTP packet generator.

### Native 48 kHz

Native 48 kHz CoreAudio playback became clean after increasing transmit scheduling margin from the early 128/64-cycle geometry to a 640-cycle TX ring with 320-cycle refill halves. The committed path also uses a 16,384-frame PCM FIFO and user-interactive transport QoS.

### Playback topology

At 44.1/48 kHz CoreAudio exposes:

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

The transport permutes this user-facing order into the FW410's raw AMDTP positions. Logic Pro hardware-validated every channel.

See [`analysis/stream-topology.md`](analysis/stream-topology.md).

## Capture

At 44.1/48 kHz the FW410 device-to-host stream contains four PCM positions plus one MIDI position. CoreAudio presents:

1. Analog In 1
2. Analog In 2
3. S/PDIF In L
4. S/PDIF In R

The proven 48 kHz capture path is:

```text
FW410 input
 -> device-to-host ISO
 -> 256-slot NuDCL receive ring
 -> 32-cycle metadata publication groups
 -> terminal-slot-confirmed completed groups
 -> DBC/CIP validation
 -> AM824 MBLA-24 decode
 -> 4-channel Float32 shared capture ring
 -> AudioServerPlugIn ReadInput
 -> CoreAudio application
```

The receive-side breakthrough was to stop treating a global scan of changed DMA slots as one coherent batch. Metadata is published in 32-cycle (~4 ms) groups, and userspace consumes a group only after its terminal receive slot changes, proving that group's update list has completed. In the validated run this eliminated the DBC gaps/reorders seen in the earlier almost-clean implementation.

The shared capture ring uses a controlled 4,096-frame (~85 ms) prefill before it becomes active so CoreAudio cannot outrun the FireWire producer during startup.

Native 44.1 capture remains to be integrated and validated.

## Project milestones

### M1 — User-space FireWire

- [x] boot/discovery
- [x] async transactions
- [x] FCP/AV/C
- [x] IRM/CMP
- [x] NuDCL RX/TX

### M2 — Transport

- [x] reusable RX/TX rings
- [x] CIP/AM824 parsing
- [x] native 48 kHz live PCM
- [x] native 44.1 kHz playback
- [x] stable 48 kHz completed-chunk capture
- [ ] long-running bus-reset-safe stream engine

### M3 — FW410 / BeBoB protocol

- [x] boot/device identity
- [x] plug and stream formation discovery
- [x] 44.1/48 rate control
- [x] M-Audio 44.1 startup quirk
- [x] basic mixer/selector probing
- [ ] complete mixer/control mapping
- [ ] MIDI byte transport validation

### M4 — Audio DevKit

- [x] playback encoder
- [x] capture decoder
- [x] PCM/shared-ring abstractions
- [x] native 44.1/48 playback engines
- [x] validated 48 kHz capture engine
- [ ] unified full-duplex transport core
- [ ] automatic lifecycle/recovery service

### M5 — CoreAudio integration

- [x] FW410 appears as a normal CoreAudio device
- [x] native 44.1/48 playback
- [x] 10 playback channels
- [x] four input channels exposed
- [x] native 48 kHz recording in Logic Pro
- [ ] full-duplex capture + real playback in one runtime
- [ ] native 44.1 capture
- [ ] controls

## Current phase

The primary next step is **full-duplex runtime integration**.

`capturebridge48000` currently owns the proven 48 kHz capture engine while transmitting correctly timed digital silence as the host-to-device keepalive. `haltransport` owns the proven real playback engines. The next milestone is to merge those paths so one transport process simultaneously consumes the 10-channel playback shared ring and produces the four-channel capture shared ring.

After 48 kHz full duplex is stable, add native 44.1 capture while preserving the FW410 post-start rate-reassertion quirk.

## Documentation

- [`analysis/current-integration-status.md`](analysis/current-integration-status.md) — current handoff and immediate next work.
- [`analysis/capture-pipeline.md`](analysis/capture-pipeline.md) — validated capture architecture and controlled quality evidence.
- [`analysis/stream-topology.md`](analysis/stream-topology.md) — raw and CoreAudio channel mapping.
- [`analysis/isochronous-transport.md`](analysis/isochronous-transport.md) — FireWire/CIP/AMDTP transport findings.
- [`HISTORY.md`](HISTORY.md) — visible hardware/software milestones.

## Release policy

The project release contract is documented in [`../RELEASES.md`](../RELEASES.md). The agreed version format is `x.yy.zzz`; future tag-driven packages are planned as `lite`, `full`, and exact-source variants.

## Disclaimer

This repository is an independent reverse-engineering and compatibility project. It is not affiliated with, endorsed by, or supported by M-Audio, Avid, Apple, or any hardware manufacturer. Vendor drivers, firmware and other proprietary material remain subject to their respective licenses and copyrights.
