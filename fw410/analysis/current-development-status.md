# FW410 current development status

Last updated: 2026-08-29

> **Superseded handoff.** This file previously described the project at the August 16 AudioDriverKit-transition stage. The implementation has advanced substantially since then and that text is no longer the canonical current state.

Use these documents instead:

- [`current-integration-status.md`](current-integration-status.md) — canonical current integration/development handoff;
- [`control-panel-roadmap.md`](control-panel-roadmap.md) — current GUI/control roadmap and completion state;
- [`original-control-panel-mixer-model.md`](original-control-panel-mixer-model.md) — validated FW410 main-mixer topology, initialization/cache rule, hardware tests and CoreAudio software-return mapping;
- [`sample-rate-lifecycle.md`](sample-rate-lifecycle.md) — current 44.1/48 kHz lifecycle;
- [`capture-pipeline.md`](capture-pipeline.md) — current capture design;
- [`stream-topology.md`](stream-topology.md) — CoreAudio/raw stream mapping.

## Current high-level state

The FW410 now operates as a normal CoreAudio device through the macfw AudioServerPlugIn/user-space transport architecture. Native 44.1/48 kHz full duplex, recovery, launchd lifecycle and the transport-owned control socket are hardware-validated.

The native AppKit control panel currently provides:

- Mixer — hardware-validated 7-source x 5-bus assignment matrix;
- Outputs — physical output source/level controls and stereo link;
- Headphones — source/level/mixer routing and stereo link;
- AUX — validated stereo level controls;
- Info — component/system/device status.

The main mixer requires a complete coherent 35-cell CONTROL initialization before differential route writes. The transport caches that state and deliberately avoids mixer STATUS polling. The GUI translates the FW410's raw software-return identities into CoreAudio/Logic channel order.

The next development checkpoint is **remaining Mixer strip controls**, not AudioDriverKit bootstrap. Follow `current-integration-status.md` for exact architecture and handoff details.
