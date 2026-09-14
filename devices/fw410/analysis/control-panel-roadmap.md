# FW410 control-panel roadmap

Last updated: 2026-09-05

This document defines the expansion plan for the native macfw FW410 control panel and records the current release-candidate boundary.

## Design rules

- Keep FireWire ownership inside the active transport engine. The GUI remains a client of transport-owned IPC/CoreAudio state.
- Do not expose controls until their FW410/AV-C semantics are understood and hardware-validated.
- Preserve full-duplex playback/capture while changing controls.
- Prefer native macOS/AppKit behavior and a compact modern layout rather than reproducing the legacy M-Audio panel pixel-for-pixel.
- Keep destructive/debug FireWire operations out of the normal control surface.
- Use Linux `snd-firewire-ctl-services`, FFADO and the original M-Audio control panel as protocol/UI references, but verify writes on real FW410 hardware.
- Treat buffer/latency as a separate engineering problem: CoreAudio buffer size, shared rings, PCM FIFO, FireWire TX geometry and capture prefill are different layers and must not be collapsed into an ambiguous slider.

## Release-candidate GUI baseline

Implemented and hardware-tested:

- native AppKit application built with Command Line Tools;
- application icon and exact build identity;
- **Mixer** tab with complete 7-source x 5-bus routing matrix;
- **Outputs** tab with five physical stereo output pairs, Mixer/AUX source selection, independent L/R level and stereo link behavior;
- **Headphones** tab with Mixer/AUX source, independent L/R volume, five mixer-output pair enables and stereo link behavior;
- **AUX** tab with software-return 1/2 and AUX output stereo levels;
- **Inputs** tab with live Analog Input 1/2 and S/PDIF L/R capture meters;
- **Device** tab with transport/CoreAudio state and 44.1/48 kHz sample-rate selection through CoreAudio;
- **Info** tab with GUI/HAL/runtime identity, runtime diagnostics, Copy Diagnostics and Open Transport Log;
- control IPC through the active transport, never direct FireWire ownership from the GUI;
- main-mixer software-return labels translated into CoreAudio/Logic order;
- multiple simultaneous mixer-bus assignments;
- analog direct-monitor assignments to multiple output buses;
- asynchronous/coalesced GUI control writes for responsive sliders and headphone controls;
- clean GUI build after Makefile/path and Objective-C++ warning cleanup.

The control panel is now complete enough for the current release scope. Remaining roadmap items are enhancements or protocol research, not release blockers.

## 1. Outputs tab — COMPLETE

The five physical stereo output pairs are exposed:

- Analog 1/2
- Analog 3/4
- Analog 5/6
- Analog 7/8
- S/PDIF L/R

Validated controls:

- per-pair Mixer/AUX source selection;
- independent L/R physical output levels;
- simultaneous stereo level writes;
- `-inf` level representation;
- S/PDIF connector state readback;
- reusable stereo-link behavior preserving the pre-link L/R offset.

## 2. Mixer tab — ROUTING COMPLETE; STRIP CONTROLS PARKED

The FW410 main mixer is confirmed as a **7 x 5 assignment matrix**.

Sources:

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

The FW410 mixer cannot safely be treated as independent unknown cells. A single isolated CONTROL write against unknown state was destructive to normal audio. The validated production sequence remains:

1. establish the complete 35-cell macfw-compatible matrix with CONTROL writes;
2. cache that state in the transport;
3. avoid mixer STATUS polling;
4. issue later changes differentially against the trusted cache.

### CoreAudio/AV-C return translation

```text
GUI SW Return 1/2   -> raw sw3/4
GUI SW Return 3/4   -> raw sw5/6
GUI SW Return 5/6   -> raw sw7/8
GUI SW Return 7/8   -> raw sw9/10
GUI SW Return 9/10  -> raw sw1/2
```

Main strip level/pan/mute/AUX-send research is **parked**. Tested FFADO Feature Volume addresses accepted writes/readback but did not prove an audible mapping to the active main-mixer paths. These controls must not be exposed until the enhanced-mixer signal topology is understood and hardware-validated.

Detailed routing evidence: [`original-control-panel-mixer-model.md`](original-control-panel-mixer-model.md).

## 3. Inputs / Monitoring — RELEASE BASELINE COMPLETE

The validated Mixer matrix already provides direct-monitor destination assignment for Analog In 1/2 and S/PDIF input.

The user-facing Inputs tab now provides live capture meters while explicitly keeping monitoring/routing semantics in Mixer.

Current Inputs surface:

- Analog Input 1 meter;
- Analog Input 2 meter;
- S/PDIF Input L meter;
- S/PDIF Input R meter;
- clear note that direct-monitor routing does not change CoreAudio capture channel assignment.

Input-specific level/pan/mute controls remain coupled to the parked strip-semantics research and are post-release work.

## 4. Live meters — COMPLETE BASELINE

Architecture:

```text
FW410 FireWire capture stream
        -> existing AMDTP decode
        -> transport-side peak/decay accumulator
        -> /tmp/macfw-fw410-meter.sock
        -> asynchronous GUI polling
        -> Inputs tab meter display
```

Validated properties:

- no additional FireWire owner;
- no AV/C/FCP meter commands;
- no change to capture SHM semantics;
- four channels in CoreAudio order: Analog 1, Analog 2, S/PDIF L, S/PDIF R;
- transport-side peak accumulation with decay;
- GUI polling asynchronously at 100 ms;
- backend dBFS values with floor and numeric display;
- no transport scheduling regression observed.

The earlier standalone **Meters** tab was intentionally replaced by the contextual **Inputs** tab for the release UI.

Future extensions may add playback/output/AUX meters if practical without affecting transport scheduling.

## 5. Device tab — COMPLETE RELEASE BASELINE

Implemented:

- connection/transport state;
- active and requested native sample rate;
- engine PID;
- CoreAudio buffer size/capability information where exposed;
- 44.1 / 48 kHz segmented control;
- rate change through `kAudioDevicePropertyNominalSampleRate` and the normal CoreAudio/HAL configuration lifecycle;
- no direct GUI `rateprobe` or FireWire ownership;
- latency/safety fields shown as **Not reported by HAL** when the current HAL returns placeholder zero values.

Known behavior:

- 44.1 -> 48 kHz is fast;
- 48 -> 44.1 kHz is noticeably slower because of the established 44.1 startup sequence;
- the slow direction completes correctly and is a release non-blocker.

Possible later Device enhancements:

- confirmed clock/source state and selection if the protocol is mapped safely;
- safe service restart/recovery action if there is a strong user need;
- improved rate-transition progress indication.

## 6. Buffer / latency control — DEFERRED

Current validated capture prefill is **256 frames**, not the earlier 4096-frame development baseline.

Relevant buffering layers include:

- CoreAudio client/device buffer frame size;
- HAL playback/capture shared rings;
- transport PCM FIFO;
- FireWire TX ring/refill geometry;
- 256-frame capture prefill;
- application/plugin buffering outside macfw.

The HAL's current latency/safety-offset values are placeholders, not calibrated measurements. The GUI therefore does not present them as real latency.

Do not expose a latency slider/profile until the complete input/output pipeline is measured and the CoreAudio property semantics are implemented correctly.

## 7. Stereo link controls — COMPLETE BASELINE

Reusable link behavior exists for headphone and physical-output stereo levels:

- linked movement preserves the pre-link L/R offset;
- enabling link does not change hardware state;
- unlinked channels remain independent;
- equalize/reset would require an explicit future action.

Apply this behavior consistently to future validated stereo controls.

## 8. Presets / state — BASE PERSISTENCE COMPLETE; NAMED PRESETS LATER

Implemented control state survives normal reconnect/rate/service lifecycle.

Named user presets remain a post-release enhancement. Candidate snapshot state:

- output routing/levels;
- mixer routing and future validated strip controls;
- headphone source/mixer/level;
- AUX state;
- future input-monitoring controls.

## 9. Optional menu-bar status — POST-RELEASE

Optional future item:

```text
FW410 • 48 kHz
```

Potential quick actions:

- connection/rate status;
- headphone source;
- headphone level;
- Open Control Panel.

Not required for the current release.

## 10. Info / diagnostics — COMPLETE RELEASE BASELINE

Implemented:

- exact GUI version/build;
- exact HAL version/build;
- exact installed runtime version/build via installer-persisted metadata;
- FW410/system/FireWire information from the existing Info surface;
- engine PID, transition count, heartbeat, capture queue, underrun and zero-fill diagnostics;
- **Copy Diagnostics** action including full transport status and recent transport log tail;
- **Open Transport Log** action.

## Ordered execution status

```text
1 Outputs                         COMPLETE
2 Mixer routing                  COMPLETE; strip controls PARKED
3 Inputs / Monitoring            RELEASE BASELINE COMPLETE
4 Live input meters              COMPLETE
5 Device                         COMPLETE RELEASE BASELINE
6 Buffer / latency reporting     DEFERRED
7 Stereo link behavior           COMPLETE BASELINE
8 Persistent state               BASE COMPLETE; named presets later
9 Optional menu-bar status       POST-RELEASE
10 Info / diagnostics            COMPLETE RELEASE BASELINE
```

## Release-candidate checkpoint

Do not add new protocol/control scope before the current release. Preserve the hardware-validated audio scheduler, mixer-state model and control IPC.

Final regression should cover:

1. clean build and full install;
2. 44.1 kHz playback/capture/Logic monitoring;
3. 48 kHz playback/capture/Logic monitoring;
4. Device-tab 44.1 -> 48 -> 44.1 switching;
5. one corresponding rate-switch test from Audio MIDI Setup;
6. Mixer, Outputs, Headphones and AUX while audio is active;
7. Inputs meters at both rates;
8. state persistence across rate switch and service restart;
9. physical disconnect/reconnect recovery;
10. Info runtime identity, Copy Diagnostics and Open Transport Log;
11. clean GUI build without the previously observed target/compiler warnings.

After this regression passes, the next project action is packaging/release preparation, only with explicit approval.

## Post-release priorities

Recommended order after the release:

1. investigate/optimize slow 48 -> 44.1 transition with stage timing;
2. reproduce and harden the separate SIGPIPE/disconnected-control-client failure if still observable;
3. calibrated CoreAudio latency/safety-offset reporting;
4. mixer strip level/pan/mute/AUX-send research;
5. named presets;
6. optional menu-bar controls.
