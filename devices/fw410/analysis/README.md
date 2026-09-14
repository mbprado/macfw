# FW410 analysis and development handoffs

This directory contains reverse-engineering evidence, hardware-validated transport findings, architecture decisions and current development handoffs for the M-Audio FireWire 410.

## Current documents

- [`current-integration-status.md`](current-integration-status.md) — current CoreAudio/HAL/runtime status and immediate next work.
- [`capture-pipeline.md`](capture-pipeline.md) — validated 48 kHz CoreAudio capture architecture, NuDCL completed-chunk solution and controlled quality evidence.
- [`current-development-status.md`](current-development-status.md) — detailed historical handoff covering boot, AV/C/CMP, packet formations, bridge experiments and architecture decisions.
- [`stream-topology.md`](stream-topology.md) — raw FW410 and CoreAudio-facing channel mappings.
- [`isochronous-transport.md`](isochronous-transport.md) — confirmed ISO/CIP/AMDTP behavior and receive-publication findings.
- [`M1-user-space-firewire.md`](M1-user-space-firewire.md) — first user-space FireWire proof milestone.
- [`bootloader-protocol.md`](bootloader-protocol.md) and [`bootloader-rom.md`](bootloader-rom.md) — boot personality and guarded boot transition findings.
- `reports/` — static-analysis and historical reports.

## Validated integration status

As of 2026-08-19:

- the AudioServerPlugIn publishes a normal `M-Audio FireWire 410` CoreAudio device;
- native 44.1 and 48 kHz playback work on hardware;
- all eight analog and both S/PDIF playback channels were validated in Logic Pro;
- native 48 kHz four-channel CoreAudio capture works;
- completed 32-cycle NuDCL receive groups eliminate the steady-state capture discontinuities seen in earlier recordings;
- the next primary step is sustained full-duplex operation in the normal rate-aware transport.

## Analysis workflow

1. Preserve original vendor artifacts unchanged.
2. Record hashes and metadata.
3. Inspect the legacy implementation only as behavioral evidence.
4. Correlate findings with Linux BeBoB/FireWire and FFADO references.
5. Implement the smallest clean macOS user-space experiment.
6. Validate every transport abstraction against real hardware.
7. Promote successful experiments into reusable library/runtime components.
8. Record failed hypotheses so they are not reopened without new evidence.

## Architecture rule

The project does not aim to reproduce the original kext architecture. CoreAudio/HAL, device control and FireWire transport remain separated, with audio crossing a narrow versioned shared-memory boundary.
