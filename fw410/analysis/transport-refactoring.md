# FW410 full-duplex transport refactoring

Last updated: 2026-08-24

This document tracks the structural consolidation of the two hardware-validated native full-duplex engines. The refactor is intentionally incremental: no rate-specific AMDTP packet behavior is moved until each common layer has been compiled and hardware-tested at both 44.1 and 48 kHz through `haltransport`.

## Validated baseline

Before refactoring, both native engines were hardware-confirmed with:

- ten CoreAudio playback channels;
- four CoreAudio capture channels;
- simultaneous playback, capture and software monitoring;
- repeated 44.1 <-> 48 kHz switching under `haltransport`.

Rate-specific behavior that must remain explicit:

| Behavior | 44.1 kHz | 48 kHz |
|---|---|---|
| cycle lead | 2048 cycles | 256 cycles |
| transmit scheduler | `AmdtpPcmStream44100` | `AmdtpPcmStream48k` |
| initial TX ring | native 44.1 blocking/NODATA schedule | native 48 kHz schedule |
| capture FDF | `0x01` | `0x02` |
| capture completion token | terminal timestamp only | terminal `(timestamp, isoHeader)` |
| playback service | one `pumpPlayback()` per loop | drain available SHM backlog |
| run-loop slice | 1 ms | 0.25 ms |
| startup quirk | post-start AV/C 44.1 reassert | none |
| transport QoS | existing behavior | user-interactive request |

These differences are treated as strategy/configuration, not accidental duplication.

## Checkpoint 1 — shared HAL/PCM plumbing: validated

`tools/transport/full_duplex_shared.h` now owns rate-independent host-side plumbing:

- playback shared-memory mapping/lifetime;
- backlog discard on engine start;
- CoreAudio physical-order -> FW410 stream-order channel permutation;
- Float32 -> signed 24-bit PCM conversion;
- common PCM/ring geometry constants;
- single-pass playback pumping and backlog draining helpers;
- FireWire cycle-count extraction.

Both native engines were rebuilt after this extraction and hardware-tested through `haltransport`. Playback, capture, monitoring and rate switching remained unchanged.

## Checkpoint 2 — shared CMP/ISO lifecycle: validated

`tools/transport/full_duplex_lifecycle.h` now owns the common FireWire connection lifecycle:

1. read and validate oPCR0/iPCR0 readiness;
2. create device-to-host and host-to-device isoch allocations;
3. bind the receive NuDCL local port and transmit NuDCL talker port;
4. install FireWire callback/isoch dispatchers and notifications;
5. allocate ISO resources;
6. connect oPCR0 then iPCR0;
7. start playback/talker ISO first, then capture ISO;
8. on shutdown, stop ISO, restore CMP registers and release allocations;
9. remove notifications and run-loop dispatchers.

The cleanup API is deliberately staged. The 44.1 kHz bridge still performs:

```text
stop ISO / restore CMP / release allocations
 -> remove FCP response space
 -> remove FireWire dispatchers
```

This preserves the known-good 44.1 AV/C/FCP lifecycle rather than hiding it inside the common helper.

After this extraction, both native engines again compiled and were hardware-tested through `haltransport`. Capture, monitoring and playback remained clean at both rates and survived rate changes in both directions. Commit `d7b2d25` is the validated checkpoint immediately after wiring the shared lifecycle into the 44.1 engine.

## Checkpoint 3 — shared full-duplex service loop: next

The remaining large duplication is the steady-state runtime loop. Both engines use the same ordering:

```text
run-loop service
 -> capture receive service
 -> CoreAudio playback SHM -> PCM service
 -> native TX scheduler service
 -> capture receive service again
 -> capture prefill activation
 -> periodic combined playback/capture diagnostics
```

The ordering is not cosmetic. Servicing RX before and after playback work is required to minimize the receive-DMA reuse window; changing that order previously made capture audibly broken in full duplex.

The common service-loop helper must therefore preserve that exact order while taking the known rate-specific differences as parameters/callbacks: run-loop slice, playback pump/drain policy, native streamer object and rate-specific banner values.

This checkpoint is **not hardware-validated yet**. After it is wired into both bridges, the required regression test is:

1. compile `halbridge44100` and `halbridge48000`;
2. start via `haltransport`;
3. verify simultaneous playback + capture/monitoring at 44.1 kHz;
4. switch to 48 kHz and repeat;
5. switch back to 44.1 kHz once;
6. confirm no audible regression and healthy capture counters.

## After the service-loop checkpoint

Once the common steady-state loop is hardware-validated, the two bridge binaries should contain mostly rate strategy and the 44.1-specific AV/C hook. Only then should the project consider collapsing the two native engines further or moving boot/recovery responsibilities into the long-running supervisor.

The next non-refactoring priorities remain automatic bootloader handling, bus-reset/generation recovery, runtime HAL build identification, and later capture-prefill/monitoring-latency tuning.