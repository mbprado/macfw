# FW410 current development status

Last updated: 2026-09-05

This file is the short current-state index. Detailed architecture and implementation notes live in the linked documents below.

Use these documents as the current handoff set:

- [`current-integration-status.md`](current-integration-status.md) — canonical integration/runtime/control-panel state;
- [`control-panel-roadmap.md`](control-panel-roadmap.md) — GUI roadmap, completed release scope and deferred work;
- [`sample-rate-lifecycle.md`](sample-rate-lifecycle.md) — current 44.1/48 kHz rate-switch/startup policy;
- [`capture-pipeline.md`](capture-pipeline.md) — current full-duplex capture path and latency baseline;
- [`original-control-panel-mixer-model.md`](original-control-panel-mixer-model.md) — validated main-mixer topology and 35-cell initialization/cache rule;
- [`stream-topology.md`](stream-topology.md) — CoreAudio/raw stream mapping.

## Current high-level state

The M-Audio FireWire 410 now operates as a normal CoreAudio device through the macfw AudioServerPlugIn plus launchd-managed user-space FireWire transport.

Hardware-validated release-candidate audio baseline:

- native 44.1 kHz full duplex;
- native 48 kHz full duplex;
- 10 playback channels and 4 capture channels;
- physical loopback / Logic software-monitoring round trip;
- low-latency capture with a 256-frame capture prefill;
- dedicated isoch callback thread;
- dedicated Mach-paced real-time audio service thread;
- `THREAD_TIME_CONSTRAINT_POLICY` audio scheduling at both rates;
- launchd restart/reload, disconnect/reconnect and guarded recovery;
- runtime 44.1 <-> 48 kHz switching through the normal CoreAudio/HAL lifecycle.

44.1 kHz and 48 kHz are both now subjectively and operationally stable in normal launchd service operation. 48 kHz currently has slightly lower perceived round-trip latency. Do not retune the proven scheduling path for this release unless a new reproducible regression requires it.

The native AppKit control panel currently provides:

- **Mixer** — hardware-validated 7-source x 5-bus assignment matrix;
- **Outputs** — physical output source/level controls and stereo link;
- **Headphones** — source/level/mixer routing and stereo link;
- **AUX** — validated stereo level controls;
- **Inputs** — live Analog 1/2 and S/PDIF L/R input meters;
- **Device** — live transport/CoreAudio state and 44.1/48 kHz sample-rate selection through CoreAudio;
- **Info** — exact build/runtime metadata, transport diagnostics, Copy Diagnostics and Open Transport Log.

The main mixer requires a coherent complete 35-cell CONTROL initialization before differential route writes. The transport caches that state and deliberately avoids mixer STATUS polling. The GUI translates the FW410's raw software-return identities into CoreAudio/Logic channel order.

## Release-candidate policy

The current audio and control paths are considered the release baseline. Remaining work before packaging is regression/packaging/documentation rather than new protocol development.

Known non-blockers/deferred work:

- 48 -> 44.1 kHz switching is noticeably slower than 44.1 -> 48 kHz because of the established 44.1 startup sequence; preserve reliability for this release and optimize later;
- HAL latency/safety-offset properties are not calibrated and currently report placeholder zero internally; the GUI deliberately displays these as **Not reported by HAL** rather than presenting false measurements;
- unresolved main-mixer strip level/pan/mute/AUX-send semantics remain parked and must not be exposed without hardware-validated protocol understanding;
- named presets and optional menu-bar controls are post-release enhancements;
- foreground `MACFW_VERBOSE=1` execution is not an authoritative performance test because verbose reporting runs on the real-time audio service path; normal launchd service operation is the release reference.

Proceed with the final regression checklist in `current-integration-status.md` / `control-panel-roadmap.md`, then package/release only after explicit approval.
