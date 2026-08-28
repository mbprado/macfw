# FW410 control-panel roadmap

Last updated: 2026-08-28

This document defines the ordered expansion plan for the native macfw FW410 control panel after the first hardware-validated Headphones/AUX/Info implementation.

## Design rules

- Keep FireWire ownership inside the active transport engine. The GUI remains a client of the transport-owned control IPC.
- Do not expose controls until their FW410/AV-C semantics are understood and hardware-validated.
- Preserve full-duplex playback/capture while changing controls.
- Prefer native macOS/AppKit behavior and a compact modern layout rather than reproducing the legacy M-Audio panel pixel-for-pixel.
- Keep transport/debug internals out of the normal mixer surface unless they are safe user-facing controls.
- Treat buffer/latency as a separate engineering problem: CoreAudio buffer size, shared rings, PCM FIFO, FireWire TX geometry and capture prefill are different layers and must not be collapsed into an ambiguous slider.

## Current GUI baseline

Implemented and hardware-tested:

- native AppKit application built with Command Line Tools, no full Xcode requirement;
- application icon and build identity;
- Headphones tab;
- Mixer/AUX headphone source selection;
- independent headphone L/R volume;
- five headphone mixer source pairs: 1/2, 3/4, 5/6, 7/8, 9/10;
- AUX software-return 1/2 stereo level;
- AUX output stereo level;
- Info tab with component/system/device information;
- headphone state preserved across 44.1 <-> 48 kHz transitions.

## 1. Outputs tab — active implementation target

Expose the five physical stereo output pairs:

- Analog 1/2
- Analog 3/4
- Analog 5/6
- Analog 7/8
- S/PDIF L/R

Hardware-validated control primitives now include:

- read-only state for all five output pairs while full-duplex audio remains active;
- per-pair Mixer/AUX source selection with hardware read-back verification;
- independent L/R physical output level writes with hardware read-back verification;
- simultaneous stereo level writes;
- `-inf` level/mute representation;
- S/PDIF connector state readback.

The GUI target is therefore:

- output source/routing selection;
- independent L/R output level;
- linked stereo level;
- mute;
- balance through independent L/R levels where appropriate.

### Stereo link / lock behavior

Each stereo output pair should have a small native lock/link button beside the L/R level controls.

- unlocked: L and R move independently;
- locked: moving either L or R moves both channels together;
- locking should not itself cause an unexpected level jump;
- while linked, preserve the existing L/R offset if the pair was unequal when locked, clamping safely at the hardware limits;
- provide an explicit way to make both channels equal when desired rather than silently doing so when the lock is enabled.

The link is GUI behavior built on the already-validated independent AV/C L/R writes; it is not a new hardware control.

Protocol work must continue to precede GUI exposure. Existing Linux/legacy-panel mappings are references, but every new write path is to be validated on the real FW410 while audio remains active.

## 2. Mixer tab

Build a modern representation of the FW410 internal mixer using compact stereo channel strips rather than cloning the old panel.

Expected sources include:

- software return 1/2;
- software return 3/4;
- software return 5/6;
- software return 7/8;
- software return 9/10 / S/PDIF return;
- analog input;
- digital input.

Candidate strip controls:

- level;
- stereo link;
- pan/balance;
- mute;
- routing/destination selection where the hardware exposes it.

## 3. Inputs / Monitoring tab

Separate recording topology from direct hardware monitoring so the UI makes clear that CoreAudio capture and zero/low-latency hardware monitoring are different paths.

Targets:

- Analog In 1/2 monitoring level;
- S/PDIF In L/R monitoring level;
- pan/balance where supported;
- monitor destination/routing;
- clear indication that these controls do not change the CoreAudio input channel assignment.

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

Success criteria include glitch-free playback/capture, correct reported latency and reliable rate/reconnect lifecycle at every exposed setting.

## 7. Stereo link controls

Generalize the output lock/link interaction to every stereo level pair for which it is useful.

- linked: moving either channel updates both while preserving the pre-link L/R offset;
- unlinked: L/R are independently adjustable and can be used for balance/pan-like behavior;
- enabling link must not itself change hardware level;
- explicit equalize/reset behavior may set both channels to the same value when requested.

This reuses the already-proven independent AV/C L/R level writes rather than inventing new hardware semantics. Point 1 implements the interaction first for physical outputs; point 7 applies the same UX consistently to the broader mixer/control panel.

## 8. Presets / state

Add named macfw mixer/control snapshots after the broader control model is complete.

Candidate saved state:

- output routing/levels;
- mixer levels/pan/mutes;
- input-monitoring state;
- headphone source/mixer/level;
- AUX state.

Hardware state persistence is useful but should not be the only persistence mechanism. A macOS-side preset can restore a known complete configuration after reconnecting or replacing an interface.

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

The full control panel remains a normal application; menu-bar-only operation is not required.

## 10. Info / diagnostics refinement

Expand the existing Info tab into a useful support surface.

Targets:

- exact GUI version/build;
- exact HAL version/build;
- exact installed transport/control runtime version/build via installer-persisted metadata;
- FW410 model/personality/GUID/firmware identity;
- FireWire controller model and negotiated bus information;
- transport state, active/requested rate, engine PID and recovery counters;
- Copy Diagnostics action producing a concise bug-report block;
- optional Open Log action.

The UI must distinguish exact component metadata from inferred/package-level metadata.

## Ordered execution

The agreed implementation sequence is:

```text
1 Outputs
2 Mixer
3 Inputs / Monitoring
4 Meters
5 Device
6 Buffer / latency investigation + implementation
7 Stereo link behavior
8 Presets / state
9 Optional menu-bar status
10 Info / diagnostics refinement
```

Some infrastructure can be shared across later items, but this order keeps the first changes on already-understood AV/C control territory and postpones timing-sensitive transport changes until the control surface is mature.

## Immediate next checkpoint

Finish point 1 by exposing the now-validated physical output controls in the AppKit GUI. Use five compact stereo output strips with source selection, L/R level, mute and a lock/link button. The lock is UI-side behavior and must not alter hardware state merely by being enabled. After the Outputs tab is hardware-tested at 44.1 and 48 kHz, move to the internal Mixer mapping.
