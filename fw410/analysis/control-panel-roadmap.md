# FW410 control-panel roadmap

Last updated: 2026-08-29

This document defines the ordered expansion plan for the native macfw FW410 control panel.

## Design rules

- Keep FireWire ownership inside the active transport engine. The GUI remains a client of the transport-owned control IPC.
- Do not expose controls until their FW410/AV-C semantics are understood and hardware-validated.
- Preserve full-duplex playback/capture while changing controls.
- Prefer native macOS/AppKit behavior and a compact modern layout rather than reproducing the legacy M-Audio panel pixel-for-pixel.
- Keep transport/debug internals out of the normal mixer surface unless they are safe user-facing controls.
- Use the Linux `snd-firewire-ctl-services` implementation and original M-Audio control panel as protocol/UI references, but verify writes on real FW410 hardware.
- Treat buffer/latency as a separate engineering problem: CoreAudio buffer size, shared rings, PCM FIFO, FireWire TX geometry and capture prefill are different layers and must not be collapsed into an ambiguous slider.

## Current GUI baseline

Implemented and hardware-tested:

- native AppKit application built with Command Line Tools, no full Xcode requirement;
- application icon and build identity;
- **Mixer** tab with the complete 7-source x 5-bus routing matrix;
- **Outputs** tab with five physical stereo output pairs, Mixer/AUX source selection, independent L/R level and stereo link behavior;
- **Headphones** tab with Mixer/AUX source, independent L/R volume, five mixer-output pair enables and stereo link behavior;
- **AUX** tab with software-return 1/2 and AUX output stereo levels;
- **Info** tab with component/system/device information;
- control IPC through the active transport, never direct FireWire ownership from the GUI;
- main-mixer software-return labels translated into CoreAudio/Logic order;
- multiple simultaneous mixer-bus assignments;
- analog direct-monitor assignments to multiple output buses.

## 1. Outputs tab — complete baseline

The five physical stereo output pairs are exposed:

- Analog 1/2
- Analog 3/4
- Analog 5/6
- Analog 7/8
- S/PDIF L/R

Hardware-validated controls include:

- per-pair Mixer/AUX source selection;
- independent L/R physical output levels;
- simultaneous stereo level writes;
- `-inf` level representation;
- S/PDIF connector state readback;
- reusable stereo-link behavior that preserves the existing L/R offset.

The current Outputs GUI has been hardware-tested across all eight analog output channels. S/PDIF-specific physical verification remains useful as an additional compatibility check, but the output-control backend and GUI baseline are complete enough to proceed.

## 2. Mixer tab — routing complete, strip controls next

The FW410 main mixer is now confirmed as a **7 x 5 assignment matrix**.

User-facing sources:

- Analog In 1/2;
- S/PDIF In L/R;
- SW Return 1/2;
- SW Return 3/4;
- SW Return 5/6;
- SW Return 7/8;
- SW Return 9/10.

Destination buses:

- 1/2;
- 3/4;
- 5/6;
- 7/8;
- S/PDIF.

The routing matrix is implemented and hardware-tested, including multiple simultaneous assignments.

### Required state-management rule

The FW410 mixer cannot safely be treated as independent unknown cells. A single isolated CONTROL write against unknown state was destructive to normal audio. The validated production sequence is:

1. establish the complete 35-cell macfw-compatible matrix with CONTROL writes;
2. cache that state in the transport;
3. avoid mixer STATUS polling;
4. issue later changes differentially against the trusted cache.

This rule must remain intact as mixer features expand.

### CoreAudio/AV-C return translation

The raw FW410 software-return identities are rotated relative to macfw's CoreAudio/AMDTP presentation. The GUI deliberately translates them:

```text
GUI SW Return 1/2   -> raw sw3/4
GUI SW Return 3/4   -> raw sw5/6
GUI SW Return 5/6   -> raw sw7/8
GUI SW Return 7/8   -> raw sw9/10
GUI SW Return 9/10  -> raw sw1/2
```

The next Mixer work is to decode and validate the remaining original strip controls, such as level, pan/balance, mute/solo and AUX sends where supported. Add one semantic control class at a time and keep the proven routing baseline untouched.

Detailed routing evidence: [`original-control-panel-mixer-model.md`](original-control-panel-mixer-model.md).

## 3. Inputs / Monitoring

The current Mixer routing already provides validated direct-monitor destination assignment for Analog In 1/2 and S/PDIF input. This phase should therefore focus on input-specific controls that are not already represented naturally by the Mixer matrix.

Targets include:

- input monitoring level;
- pan/balance where supported;
- mute/solo semantics where confirmed;
- clear indication that these controls do not change CoreAudio input channel assignment.

## 4. Live meters

Add lightweight peak/RMS metering after the control mappings are stable.

Preferred architecture:

```text
already-decoded playback/capture PCM
        -> transport-side peak/RMS accumulator
        -> lightweight shared status / control IPC
        -> GUI meters
```

This avoids a second FireWire client and avoids polling hardware for information already present in the transport.

Initial useful meters:

- Analog In 1/2;
- S/PDIF In L/R;
- software playback/output pairs;
- AUX where practical.

Metering must not compromise the transport scheduling margins that currently keep full-duplex audio stable.

## 5. Device tab

Add user-facing device controls and state:

- current native sample rate;
- 44.1 / 48 kHz selection through the normal CoreAudio/HAL lifecycle, not by bypassing it;
- clock/source state if protocol mapping is confirmed;
- connection/transport state;
- safe transport restart/recovery action if useful.

Debug-only or destructive FireWire operations should remain outside the normal panel.

## 6. Buffer / latency control

Investigate before implementing UI.

Relevant buffering layers currently include:

- CoreAudio client/device buffer frame size;
- HAL playback/capture shared rings;
- transport PCM FIFO;
- FireWire TX ring/refill geometry;
- capture prefill (currently 4096 frames).

First question: whether the AudioServerPlugIn should expose a normal CoreAudio buffer-frame-size property and what ranges the transport can sustain at both 44.1 and 48 kHz.

Only after measurement should the panel expose user choices such as 32/64/128/256/512 frames or named latency profiles. Internal FireWire geometry should remain hidden unless a validated profile genuinely needs to change it.

## 7. Stereo link controls

The reusable link behavior now exists for headphone and physical-output stereo levels.

Continue applying the same behavior to later stereo controls where useful:

- linked movement preserves the pre-link L/R offset;
- enabling link does not change hardware state;
- unlinked channels remain independent;
- any equalize/reset action must be explicit.

## 8. Presets / state

Add named macfw mixer/control snapshots after the broader control model is complete.

Candidate saved state:

- output routing/levels;
- mixer routing/levels/pan/mutes;
- input-monitoring state;
- headphone source/mixer/level;
- AUX state.

A macOS-side preset can restore a known complete configuration after reconnecting or replacing an interface.

## 9. Optional menu-bar status

After the main application is mature, optionally expose a lightweight menu-bar item such as:

```text
FW410 • 48 kHz
```

Useful quick actions:

- connection/rate status;
- headphone source;
- headphone level;
- Open Mixer.

## 10. Info / diagnostics refinement

Expand the existing Info tab into a useful support surface.

Targets:

- exact GUI version/build;
- exact HAL version/build;
- exact installed transport/control runtime version/build via installer-persisted metadata;
- FW410 model/personality/GUID/firmware identity;
- FireWire controller model and negotiated bus information;
- transport state, active/requested rate, engine PID and recovery counters;
- Copy Diagnostics action;
- optional Open Log action.

## Ordered execution

```text
1 Outputs                         COMPLETE BASELINE
2 Mixer routing                  COMPLETE; strip controls next
3 Inputs / Monitoring
4 Meters
5 Device
6 Buffer / latency investigation
7 Stereo link behavior           BASE IMPLEMENTED
8 Presets / state
9 Optional menu-bar status
10 Info / diagnostics refinement
```

## Immediate next checkpoint

Preserve the current known-good routing implementation and continue point 2 with the remaining original mixer-strip controls. Start from the Linux driver/original-panel definitions, implement software/protocol semantics before GUI exposure, and hardware-test each new write class while audio remains active.

A secondary cleanup target is to replace the GUI's current many-process mixer refresh with a single cached-matrix query once that optimization can be done without changing the proven control semantics.
