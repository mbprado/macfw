# M-Audio FireWire 410 — Modern macOS Driver / DevKit

Reverse-engineering and user-space driver/DevKit project for the M-Audio FireWire 410 (FW410).

## Goal

The goal is not to port the original M-Audio kext. The project is building a modern macOS audio stack that keeps the CoreAudio-facing driver separate from a user-space FireWire transport service and can eventually be reused for other legacy FireWire audio interfaces.

```text
CoreAudio application
        |
macfw AudioServerPlugIn
        |
versioned shared-memory audio/status ABI
        |
macfw transport service
        |
AV/C / CMP / CIP / AMDTP / NuDCL / IOFireWireLib
        |
M-Audio FireWire 410
```

Live controls use the same transport ownership:

```text
macfw FW410 Control.app / fw410ctl
        |
/tmp/macfw-fw410-control.sock
        |
active transport engine
        |
FW410 AV/C
```

The HAL and GUI never open FireWire independently.

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
- AUX source/output level controls;
- physical output source and L/R level controls;
- the complete 7-source x 5-bus main-mixer routing matrix;
- multiple simultaneous mixer-bus assignments;
- native AppKit control-panel operation for Mixer, Outputs, Headphones, AUX and system/device information;
- persistent writable hardware state across reboot and physical disconnect/reconnect;
- Reset Defaults to the documented macfw baseline;
- complete `.pkg` installation and postinstall lifecycle with the interface becoming operational without reboot.

The transport-status ABI explicitly reports `OFFLINE`, `RECOVERING`, and `ONLINE`, plus requested/active rate, native-engine PID, transition sequence and heartbeat. A native engine is not published `ONLINE` until it explicitly reports READY after successful transport startup and saved control-state restoration has been attempted.

Detailed capture design and evidence: [`analysis/capture-pipeline.md`](analysis/capture-pipeline.md).

## Control panel and persistence

The native macOS control panel is built with AppKit/Objective-C++ and the standard Command Line Tools rather than requiring full Xcode. It uses the validated `fw410ctl -> socket -> transport -> AV/C` path while audio remains owned by the active transport engine.

Current tabs:

- **Mixer** — seven sources into five mixer buses, with multiple simultaneous route assignments;
- **Outputs** — five physical stereo output pairs with Mixer/AUX source, independent L/R level and link behavior;
- **Headphones** — mixer/AUX source, independent L/R level, five mixer-output pair enables and link behavior;
- **AUX** — software return 1/2 -> AUX and AUX output stereo levels;
- **Info** — macfw/HAL build information, transport state/rate, macOS/Mac identity, FireWire controller information and FW410 identity.

The GUI intentionally presents software returns in CoreAudio/Logic order. The FW410's raw AV/C software-return identities are rotated relative to macfw's AMDTP channel order, so the GUI translates them rather than exposing confusing raw names.

Successful writable GUI/CLI control changes are recorded by the installed `fw410state` helper in `/Library/Application Support/macfw/fw410/control-state.conf`. After a native engine reaches low-level readiness, the supervisor restores saved controls before publishing `ONLINE`. Main-mixer routes are restored first through the validated complete-baseline path, preserving the same mixer-safety rule used during normal operation.

The GUI's **Reset Defaults** action applies and persists the documented macfw baseline. It is deliberately described as a macfw default rather than an undocumented M-Audio factory reset.

See [`analysis/control-state-persistence.md`](analysis/control-state-persistence.md) and [`analysis/control-panel-roadmap.md`](analysis/control-panel-roadmap.md).

## Main mixer discovery

The original M-Audio control panel and Linux `snd-firewire-ctl-services` implementation establish the FW410 main mixer as seven sources feeding five independent mixer buses.

The production routing backend follows a hardware-validated rule:

1. initialize all 35 route cells to one known macfw-compatible baseline;
2. cache that matrix in the transport process;
3. do not use mixer STATUS polling to reconstruct it;
4. perform later route changes as differential CONTROL writes.

This is required because a single isolated mixer write against unknown/default state was observed to kill normal playback, while the same differential write is safe after the full coherent initialization.

The original/Linux identity matrix is a useful logical reference, but macfw's AMDTP slot ordering requires this raw baseline:

```text
raw SW Return 3/4   -> Mixer 1/2
raw SW Return 5/6   -> Mixer 3/4
raw SW Return 7/8   -> Mixer 5/6
raw SW Return 9/10  -> Mixer 7/8
raw SW Return 1/2   -> Mixer S/PDIF
```

The GUI translates this back into the expected CoreAudio/Logic names so `SW Return 1/2` controls Logic 1/2, and so on through 9/10.

Hardware validation also confirmed analog direct monitoring, for example:

```text
Analog In 1/2 -> Mixer 1/2 => Input 1 -> Output 1, Input 2 -> Output 2
Analog In 1/2 -> Mixer 3/4 => Input 1 -> Output 3, Input 2 -> Output 4
```

See [`analysis/original-control-panel-mixer-model.md`](analysis/original-control-panel-mixer-model.md).

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

The shared capture ring uses a controlled 4,096-frame prefill (~85 ms at 48 kHz, ~93 ms at 44.1 kHz) before it becomes active so CoreAudio cannot outrun the FireWire producer during startup.

Both native rates are hardware-validated for clear recording and live monitoring.

## Disconnect/reconnect behavior

The logical **M-Audio FireWire 410** CoreAudio device intentionally remains registered when the physical FireWire interface disappears. While transport is unavailable, the HAL can remain logically present and provide silence/empty capture rather than forcing applications such as Logic to lose their selected device.

`haltransport` detects FireWire generation changes, tears down the current native engine, enters recovery/backoff, handles the FW410 bootloader personality through guarded `fwboot`, and launches a fresh native engine after the operational device returns. Hardware tests confirmed playback and capture resume after reconnection without restarting Logic, and current writable hardware controls are restored before the recovered engine is published `ONLINE`.

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
- [x] AUX level mapping
- [x] physical output source/level mapping
- [x] main-mixer 7x5 route assignment mapping
- [x] persistent writable control state
- [ ] remaining mixer strip controls
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
- [x] native AppKit control panel
- [x] Outputs controls
- [x] main-mixer routing controls
- [x] persistent controls + Reset Defaults
- [x] packaged control panel/runtime installation lifecycle
- [ ] remaining mixer strip controls
- [ ] input-specific controls
- [ ] live meters
- [ ] buffer/latency control after dedicated investigation

## Current phase

The audio transport/recovery architecture, core routing/control surface, persisted control-state lifecycle and normal package installation path are hardware-validated. Development can now continue with the remaining original control-surface semantics without changing the proven FireWire ownership and mixer initialization architecture.

The control-panel roadmap intentionally keeps latency tuning later. Metering can be derived from already-decoded PCM without requiring extra FireWire ownership. Buffer/latency controls remain a separate investigation because the stack contains multiple buffering layers and should not expose an ambiguous or unsafe generic buffer slider.

## Build/install

From the repository root:

```bash
make             # HAL + release runtime + GUI
make runtime     # installed runtime/control binaries only
make gui         # GUI only
make all-tools   # all development tools
make package     # complete installer package
sudo make install
```

`sudo make install` expects the artifacts to have already been built as the normal user. The complete source install includes `/Applications/macfw FW410 Control.app`, the persistent state helper, HAL and launchd/runtime tree.

## Documentation

- [`analysis/current-integration-status.md`](analysis/current-integration-status.md) — current handoff and immediate next work.
- [`analysis/control-panel-roadmap.md`](analysis/control-panel-roadmap.md) — ordered GUI/control expansion plan.
- [`analysis/control-state-persistence.md`](analysis/control-state-persistence.md) — persisted writable state, restore ordering and Reset Defaults.
- [`analysis/original-control-panel-mixer-model.md`](analysis/original-control-panel-mixer-model.md) — validated main-mixer model, initialization rule and GUI return mapping.
- [`analysis/headphone-control.md`](analysis/headphone-control.md) — validated headphone/AUX control model and CLI surface.
- [`analysis/sample-rate-lifecycle.md`](analysis/sample-rate-lifecycle.md) — validated 44.1/48 kHz setup, restart and cleanup policy.
- [`analysis/capture-pipeline.md`](analysis/capture-pipeline.md) — validated capture architecture and controlled quality evidence.
- [`analysis/stream-topology.md`](analysis/stream-topology.md) — raw and CoreAudio channel mapping.
- [`analysis/isochronous-transport.md`](analysis/isochronous-transport.md) — FireWire/CIP/AMDTP transport findings.
- [`HISTORY.md`](HISTORY.md) — visible hardware/software milestones.

## Release policy

The project release contract is documented in [`../RELEASES.md`](../RELEASES.md). The agreed version format is `x.yy.zzz`.

## Disclaimer

This repository is an independent reverse-engineering and compatibility project. It is not affiliated with, endorsed by, or supported by M-Audio, Avid, Apple, or any hardware manufacturer. Vendor drivers, firmware and other proprietary material remain subject to their respective licenses and copyrights.
