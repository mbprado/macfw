# FW410 current integration status

Last updated: 2026-08-29

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
- AUX source/output stereo levels;
- physical output Mixer/AUX source selection and independent L/R output levels;
- reusable stereo-link GUI behavior for headphone/output level pairs;
- complete 7x5 FW410 main-mixer assignment routing for analog input, S/PDIF input and software returns;
- multiple simultaneous mixer-bus assignments;
- CoreAudio/Logic-aligned software-return labels in the GUI despite the FW410 raw AV/C return rotation.

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
    -> /tmp/macfw-fw410-control.sock
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
active native transport engine
        |
        v
existing FireWire handle + FCP response space
        |
        v
FW410 AV/C function blocks
```

Validated controls now include:

- headphone source: mixer / AUX;
- headphone independent L/R level;
- headphone mixer sources: 1/2, 3/4, 5/6, 7/8, 9/10;
- AUX software-return 1/2 independent L/R level;
- AUX output independent L/R level;
- physical output source selection for Analog 1/2, 3/4, 5/6, 7/8 and S/PDIF;
- independent physical output L/R levels;
- S/PDIF connector state readback;
- main-mixer route assignment for seven sources into five mixer buses.

The AUX path is independent of the five-source headphone mixer. Headphone source/mixer/level state survives 44.1 <-> 48 kHz transitions.

Detailed headphone/output mapping remains in `analysis/headphone-control.md` and related protocol notes. Main-mixer evidence is consolidated in `analysis/original-control-panel-mixer-model.md`.

## Main mixer state model

The original M-Audio control panel and upstream Linux `snd-firewire-ctl-services` implementation establish a 7-source x 5-destination assignment matrix.

Sources:

- Analog In 1/2;
- S/PDIF In L/R;
- five software-return stereo pairs.

Destinations:

- Mixer 1/2;
- Mixer 3/4;
- Mixer 5/6;
- Mixer 7/8;
- Mixer S/PDIF.

A route ON is encoded as `0x0000`; OFF is `0x8000`.

Hardware testing established an important state-management rule: issuing one mixer CONTROL write against an unknown/default matrix can disable the normal audio path, while writing a complete coherent 35-cell matrix is safe. The production backend therefore:

1. lazily establishes the complete macfw-compatible 35-cell baseline on first main-mixer access;
2. caches that matrix in the transport process;
3. avoids mixer STATUS polling;
4. performs subsequent route changes as differential CONTROL writes against the trusted cache.

The Linux/original identity matrix is logically correct for the original return numbering but shifts macfw playback because macfw's AMDTP slot order is different. The validated macfw baseline compensates for that raw slot order.

Hardware tests confirmed analog direct-monitor assignments such as:

```text
Analog Input 1/2 -> Mixer 1/2  => Input 1 -> Output 1, Input 2 -> Output 2
Analog Input 1/2 -> Mixer 3/4  => Input 1 -> Output 3, Input 2 -> Output 4
```

The same matrix supports multiple simultaneous destination assignments.

## Native control panel

`fw410/control-panel` is a native AppKit/Objective-C++ application built directly with the standard macOS Command Line Tools. Full Xcode is not required.

Current tabs:

- **Mixer** — 7x5 main-mixer assignment matrix;
- **Outputs** — physical-output Mixer/AUX source selection, L/R level and link behavior;
- **Headphones** — source, L/R volume, five mixer-output pair switches and link behavior;
- **AUX** — software-return 1/2 -> AUX and AUX output stereo levels;
- **Info** — GUI/HAL build information, live transport state/rate, macOS/Mac information, FireWire controller information and FW410 identity.

The GUI currently uses the validated `fw410ctl` command as its backend boundary. This deliberately reuses the proven socket/control path while the UI evolves. Direct socket IPC can replace the subprocess boundary later without changing the hardware-control semantics.

The Mixer tab presents software returns in CoreAudio/Logic order. Internally, the raw FW410 AV/C return identities are rotated:

```text
GUI / CoreAudio SW Return 1/2   -> raw AV/C sw3/4
GUI / CoreAudio SW Return 3/4   -> raw AV/C sw5/6
GUI / CoreAudio SW Return 5/6   -> raw AV/C sw7/8
GUI / CoreAudio SW Return 7/8   -> raw AV/C sw9/10
GUI / CoreAudio SW Return 9/10  -> raw AV/C sw1/2
```

This mapping is a presentation/backend translation only; no extra FireWire ownership is introduced.

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
| physical output controls | validated | validated |
| main-mixer assignment routing | validated | validated |
| native GUI controls | validated | validated |

## Build/install status

The aggregate Makefile targets have been normalized:

```text
make             -> HAL + release runtime + GUI
make hal         -> HAL only
make runtime     -> installed runtime/control binaries only
make gui         -> control panel only
make all-tools   -> all development/reverse-engineering tools
make package     -> complete package including GUI
```

`sudo make install` now expects all installable artifacts to have been built as the normal user. It installs the HAL bundle, launchd/runtime tree and `/Applications/macfw FW410 Control.app` without intentionally compiling as root.

## Control-panel implementation sequence

1. Outputs — **routing/level GUI implemented and hardware-tested**.
2. Mixer — **7x5 assignment matrix implemented and hardware-tested**; remaining strip controls still pending.
3. Inputs / Monitoring — direct-monitor controls separate from CoreAudio capture where they are not already naturally represented by Mixer routing.
4. Meters — derive peak/RMS from already-decoded PCM and publish lightweight telemetry.
5. Device — sample-rate and confirmed clock/device controls.
6. Buffer / latency — investigate CoreAudio buffer property and each internal buffering layer before exposing settings.
7. Stereo link — base reusable behavior implemented; expand consistently where additional stereo level controls are added.
8. Presets / state — complete mixer/routing snapshots.
9. Optional menu-bar status/quick headphone controls.
10. Info / diagnostics — exact installed runtime metadata, recovery counters and Copy Diagnostics.

## Immediate next checkpoint

The routing portion of the Mixer phase is complete and hardware-validated. Before adding the remaining mixer-strip controls, preserve the current known-good state and continue with the Linux/original-control-panel protocol references for level/pan/mute semantics.

Do not change the proven 35-cell initialization rule casually. New main-mixer controls should continue through the transport-owned socket/cache architecture and should be validated one class at a time while full-duplex audio remains active.

Do not expose a generic buffer slider during this phase. Buffer/latency work remains a separate measured investigation.

## Quick handoff

> Native 44.1/48 kHz full duplex, recovery and production launchd lifecycle are hardware-validated. The transport owns the live control IPC used by `fw410ctl` and the native AppKit GUI. Headphone, AUX, physical Outputs and the complete 7x5 main-mixer assignment matrix are now hardware-validated. The main mixer requires a coherent 35-cell cached baseline before individual route writes. The GUI translates raw FW410 software-return identities into CoreAudio/Logic channel order. Current tabs are Mixer, Outputs, Headphones, AUX and Info. The next Mixer work is the remaining strip controls, using the Linux driver and original-panel model as references without changing FireWire ownership.
