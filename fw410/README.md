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

- native 44.1 kHz and 48 kHz full-duplex operation;
- all 10 playback channels: Analog Out 1-8 and S/PDIF L/R;
- all four capture channels: Analog In 1/2 and S/PDIF In L/R;
- simultaneous playback, recording and live monitoring in Logic Pro;
- runtime 44.1 <-> 48 kHz switching through `haltransport`;
- physical disconnect/reconnect with automatic transport recovery;
- logical CoreAudio-device continuity while the physical FW410 transport is offline;
- live headphone source, level and five-pair mixer control through the transport-owned control IPC;
- a native AppKit control panel for headphone, AUX and system/device information.

The transport-status ABI explicitly reports `OFFLINE`, `RECOVERING`, and `ONLINE`, plus requested/active rate, native-engine PID, transition sequence and heartbeat. A native engine is not published `ONLINE` until it explicitly reports READY after successful transport startup.

Detailed capture design and evidence: [`analysis/capture-pipeline.md`](analysis/capture-pipeline.md).

## Control panel

The current native macOS control panel is intentionally built with AppKit/Objective-C++ and the standard Command Line Tools rather than requiring full Xcode. It uses the validated control API while audio remains owned by the active transport engine.

Current tabs:

- **Headphones** — mixer/AUX source, independent L/R level and five mixer-output pair enables;
- **AUX** — software return 1/2 -> AUX and AUX output stereo levels;
- **Info** — macfw/HAL build information, transport state/rate, macOS/Mac identity, FireWire controller information and FW410 identity.

The planned expansion order is documented in [`analysis/control-panel-roadmap.md`](analysis/control-panel-roadmap.md). In short: Outputs -> Mixer -> Inputs/Monitoring -> Meters -> Device -> Buffer/Latency -> stereo linking -> presets -> optional menu-bar status -> diagnostics refinement.

## Boot identity

```text
before: FW Bootloader / model 0x00010058
after:  FW 410        / model 0x00010046
```

`haltransport` handles this personality transition during physical reconnect recovery, reacquiring fresh FireWire generation/node state and issuing the guarded boot cue only when the known FW410 loader preflight matches.

## Playback

### Native 44.1 kHz

Native 44.1 playback is hardware-confirmed. The FW410 requires an M-Audio-specific startup ritual:

1. read both AV/C signal-format directions;
2. if needed, select 44100 in both directions and verify by STATUS readback;
3. establish duplex CMP/ISO;
4. begin valid native 44.1 AMDTP traffic;
5. while duplex streaming is live, reassert 44100 on OUTPUT plug 0 and INPUT plug 0;
6. continue normal data-bearing scheduling.

If both directions are already at 44100, the initial redundant AV/C CONTROL is skipped, but the proven post-start reassert remains mandatory.

A clean 44.1 kHz supervisor stop leaves the FW410 at 44.1 kHz instead of forcing the development-era 48 kHz restore. Repeated hardware validation showed faster, consistent restarts with the expected `OFFLINE -> RECOVERING -> ONLINE` sequence and one successful native-engine PID per restart. Abnormal 44.1 engine failure retains the conservative best-effort 48 kHz recovery restore.

Detailed rate lifecycle: [`analysis/sample-rate-lifecycle.md`](analysis/sample-rate-lifecycle.md).

### Native 48 kHz

Native 48 kHz CoreAudio playback became clean after increasing transmit scheduling margin from the early 128/64-cycle geometry to a 640-cycle TX ring with 320-cycle refill halves. The committed path also uses a 16,384-frame PCM FIFO and user-interactive transport QoS.

Rate setup is idempotent at 48 kHz as well: if both AV/C directions already report 48000, no redundant rate CONTROL is sent.

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

The proven capture path is:

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

The receive-side breakthrough was to stop treating a global scan of changed DMA slots as one coherent batch. Metadata is published in 32-cycle (~4 ms) groups, and userspace consumes a group only after its terminal receive slot changes, proving that group's update list has completed.

Completion detection is rate-aware: 48 kHz uses timestamp + ISO-header state, while 44.1 kHz uses the stable timestamp token required by its alternating blocking/NODATA packet pattern.

The shared capture ring uses a controlled 4,096-frame prefill (~85 ms at 48 kHz, ~93 ms at 44.1 kHz) before it becomes active so CoreAudio cannot outrun the FireWire producer during startup.

Both native rates are hardware-validated for clear recording and live monitoring. In steady state the expected capture cadence is approximately 96,000 frames per two seconds at 48 kHz and 88,200 at 44.1 kHz.

## Disconnect/reconnect behavior

The logical **M-Audio FireWire 410** CoreAudio device intentionally remains registered when the physical FireWire interface disappears. While transport is unavailable, the HAL can remain logically present and provide silence/empty capture rather than forcing applications such as Logic to lose their selected device.

`haltransport` detects FireWire generation changes, tears down the current native engine, enters recovery/backoff, handles the FW410 bootloader personality through guarded `fwboot`, and launches a fresh native engine after the operational device returns. Hardware tests confirmed playback and capture resume after reconnection without restarting Logic.

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
- [x] native 44.1/48 kHz playback
- [x] stable completed-chunk capture at 44.1/48 kHz
- [x] native full-duplex transport at 44.1/48 kHz
- [x] rate-aware supervisor
- [x] physical disconnect/reconnect recovery

### M3 — FW410 / BeBoB protocol

- [x] boot/device identity
- [x] plug and stream formation discovery
- [x] 44.1/48 rate control
- [x] M-Audio 44.1 startup quirk
- [x] guarded bootloader recovery
- [x] headphone source/level/mixer mapping
- [x] initial AUX level mapping
- [ ] complete mixer/output/input control mapping
- [ ] MIDI byte transport validation

### M4 — Audio DevKit

- [x] playback encoder
- [x] capture decoder
- [x] PCM/shared-ring abstractions
- [x] native 44.1/48 full-duplex engines
- [x] shared full-duplex transport lifecycle
- [x] transport availability/status ABI
- [x] automatic supervisor lifecycle/recovery

### M5 — CoreAudio integration and control panel

- [x] FW410 appears as a normal CoreAudio device
- [x] native 44.1/48 playback
- [x] 10 playback channels
- [x] four input channels
- [x] native 44.1/48 recording in Logic Pro
- [x] simultaneous playback/capture
- [x] HAL remains logically present through physical disconnect/reconnect
- [x] live transport-owned control IPC
- [x] native AppKit Headphones/AUX/Info control panel
- [ ] Outputs controls
- [ ] full mixer controls
- [ ] input/direct-monitor controls
- [ ] live meters
- [ ] buffer/latency control after dedicated investigation

## Current phase

The audio transport and recovery architecture is hardware-validated at both supported native rates. Development is now expanding the production control surface behind the same transport-owned IPC that already allows headphone and AUX controls to coexist with full-duplex audio.

The control-panel roadmap intentionally prioritizes proven hardware controls before latency tuning. Output routing/levels are next, followed by the wider mixer and direct-monitor paths. Metering can then be added from already-decoded PCM without requiring extra FireWire ownership. Buffer/latency controls remain a separate investigation because the stack contains multiple buffering layers and should not expose an ambiguous or unsafe generic buffer slider.

## Documentation

- [`analysis/current-integration-status.md`](analysis/current-integration-status.md) — current handoff and immediate next work.
- [`analysis/control-panel-roadmap.md`](analysis/control-panel-roadmap.md) — ordered ten-point GUI/control expansion plan.
- [`analysis/headphone-control.md`](analysis/headphone-control.md) — validated headphone/AUX control model and CLI surface.
- [`analysis/sample-rate-lifecycle.md`](analysis/sample-rate-lifecycle.md) — validated 44.1/48 kHz setup, restart and cleanup policy.
- [`analysis/capture-pipeline.md`](analysis/capture-pipeline.md) — validated capture architecture and controlled quality evidence.
- [`analysis/stream-topology.md`](analysis/stream-topology.md) — raw and CoreAudio channel mapping.
- [`analysis/isochronous-transport.md`](analysis/isochronous-transport.md) — FireWire/CIP/AMDTP transport findings.
- [`HISTORY.md`](HISTORY.md) — visible hardware/software milestones.

## Release policy

The project release contract is documented in [`../RELEASES.md`](../RELEASES.md). The agreed version format is `x.yy.zzz`; future tag-driven packages are planned as `lite`, `full`, and exact-source variants.

## Disclaimer

This repository is an independent reverse-engineering and compatibility project. It is not affiliated with, endorsed by, or supported by M-Audio, Avid, Apple, or any hardware manufacturer. Vendor drivers, firmware and other proprietary material remain subject to their respective licenses and copyrights.
