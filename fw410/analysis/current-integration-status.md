# FW410 current integration status

Last updated: 2026-08-28

This document is the current handoff for the CoreAudio/HAL/runtime/control-panel phase. Older reverse-engineering documents remain useful historical evidence, but this file defines the present integration state and immediate next work.

## Executive status

macfw publishes **M-Audio FireWire 410** as a normal macOS CoreAudio device through a dependency-free AudioServerPlugIn.

Hardware-confirmed:

- native 44.1 kHz and 48 kHz full-duplex operation;
- ten playback channels: Analog Out 1-8 and S/PDIF L/R;
- four capture channels: Analog In 1/2 and S/PDIF In L/R;
- simultaneous playback, recording and live monitoring;
- runtime 44.1 <-> 48 kHz switching;
- physical disconnect/reconnect and guarded bootloader recovery;
- logical CoreAudio-device continuity while the physical transport is offline;
- launchd service restart/recovery, including process kill and late interface connection after macOS boot;
- native AppKit control panel operation while full-duplex audio remains active;
- headphone mixer/AUX source selection, independent L/R headphone volume and five mixer-output source pairs;
- initial AUX source/output stereo levels;
- headphone state persistence across native-rate changes.

The current architecture is:

```text
macOS application
    -> CoreAudio
    -> macfw AudioServerPlugIn
       -> playback shared ring
       <- capture shared ring
       <- transport status
    -> haltransport launchd supervisor
       -> OFFLINE / RECOVERING / ONLINE status ABI
       -> native-engine READY handshake
       -> native 44.1 or 48 full-duplex engine
          -> local FW410 control IPC
          -> AV/C control transactions
          -> CMP / ISO / AMDTP transport
    -> M-Audio FireWire 410

macfw FW410 Control.app / fw410ctl
    -> control IPC owned by active native engine
```

The HAL and GUI never open FireWire independently. Device boot, rate control, CMP, ISO, AMDTP scheduling, recovery and live AV/C control remain in the transport/service layer.

## Audio/runtime status

The validated transport uses a 640-cycle TX ring with 320-cycle refill halves, 16,384-frame playback PCM FIFO and a controlled 4,096-frame capture prefill. The receive path consumes terminal-slot-confirmed 32-cycle NuDCL groups and uses rate-specific completion semantics.

44.1 kHz retains its required post-start AV/C rate reassert before the native engine reports READY. A clean 44.1 stop leaves the interface at 44.1 rather than forcing the old development 48 kHz restore. This substantially improved repeated 44.1 restart behavior.

The logical CoreAudio device stays registered during a physical disconnect. Playback is safely discarded/silenced and capture supplies silence until the transport returns ONLINE; existing clients can continue after automatic recovery.

## Control subsystem

The production control architecture avoids the earlier conflict where standalone probes attempted to open the FireWire interface while the transport already owned it.

```text
fw410ctl / native GUI
        |
        v
/tmp/macfw-fw410-control.sock
        |
        v
active halbridge
        |
        v
existing FireWire handle + FCP response space
        |
        v
FW410 AV/C function blocks
```

Validated controls:

- headphone source: mixer / AUX;
- headphone independent L/R level;
- headphone mixer sources: 1/2, 3/4, 5/6, 7/8, 9/10;
- AUX software-return 1/2 independent L/R level;
- AUX output independent L/R level.

The AUX path is independent of the five-source headphone mixer. Headphone source/mixer/level state survives 44.1 <-> 48 kHz transitions.

Detailed mapping: `analysis/headphone-control.md`.

## Native control panel

`fw410/control-panel` is a native AppKit/Objective-C++ application built directly with the standard macOS Command Line Tools. Full Xcode is not required.

Current tabs:

- **Headphones** — source, L/R volume, five mixer-output pair switches;
- **AUX** — software-return 1/2 -> AUX and AUX output stereo levels;
- **Info** — GUI/HAL build information, live transport state/rate, macOS/Mac information, FireWire controller information and FW410 identity.

The GUI currently uses the validated `fw410ctl` command as its backend boundary. This deliberately reuses the proven control path while the UI evolves. Direct socket IPC can replace the subprocess boundary later without changing the hardware-control semantics.

The agreed ten-point expansion plan is in `analysis/control-panel-roadmap.md`.

## Current functional matrix

| Capability | 44.1 kHz | 48 kHz |
|---|---|---|
| 10-channel playback | validated | validated |
| 4-channel capture | validated | validated |
| simultaneous full duplex | validated | validated |
| software monitoring | validated | validated |
| runtime rate switching | validated | validated |
| physical disconnect/reconnect recovery | validated | validated |
| guarded bootloader recovery | validated | validated |
| transport-status / READY lifecycle | validated | validated |
| offline silence + transparent recovery | validated | validated |
| live headphone/AUX controls | validated | validated |
| headphone state across rate change | validated | validated |
| native GUI controls | validated | validated |

## Control-panel implementation sequence

1. Outputs — map/read first, then narrowly validate source/level/mute writes for Analog 1/2, 3/4, 5/6, 7/8 and S/PDIF.
2. Mixer — software returns plus analog/digital input strips.
3. Inputs / Monitoring — direct-monitor controls separate from CoreAudio capture.
4. Meters — derive peak/RMS from already-decoded PCM and publish lightweight telemetry.
5. Device — sample-rate and confirmed clock/device controls.
6. Buffer / latency — investigate CoreAudio buffer property and each internal buffering layer before exposing settings.
7. Stereo link — GUI behavior built on proven independent L/R writes.
8. Presets / state — complete mixer/routing snapshots.
9. Optional menu-bar status/quick headphone controls.
10. Info / diagnostics — exact installed runtime metadata, recovery counters and Copy Diagnostics.

## Immediate next checkpoint

Start the **Outputs** phase without changing audio transport timing:

1. inventory existing output-related selector/feature findings from repository probes and protocol references;
2. define semantic output controls in the common control server;
3. expose read-only state through `fw410ctl` first;
4. hardware-test reads while full-duplex audio is active;
5. add one guarded write class at a time (source, level, mute as confirmed);
6. validate at 44.1 and 48 kHz and across a rate transition;
7. only then add the Outputs tab to the GUI.

Do not expose a generic buffer slider during this phase. Buffer/latency work is deliberately point 6 because it can affect the scheduling margins that made playback/capture stable.

## Quick handoff

> Native 44.1/48 kHz full duplex, recovery and production launchd lifecycle are hardware-validated. The transport now also owns a live control IPC used by `fw410ctl` and the native AppKit GUI. Headphone source/level/five-pair mixer and initial AUX controls are validated while audio is active and survive native-rate switching. The GUI currently has Headphones, AUX and Info tabs. The agreed next phase is output-control mapping: read-only semantic controls first, then narrowly validated writes, then the Outputs GUI tab. The full ten-point plan is `analysis/control-panel-roadmap.md`.
