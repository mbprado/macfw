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

`tools/transport/full_duplex_shared.h` owns rate-independent host-side plumbing:

- playback shared-memory mapping/lifetime;
- backlog discard on engine start;
- CoreAudio physical-order -> FW410 stream-order channel permutation;
- Float32 -> signed 24-bit PCM conversion;
- common PCM/ring geometry constants;
- single-pass playback pumping and backlog draining helpers;
- FireWire cycle-count extraction.

Both native engines were rebuilt after this extraction and hardware-tested through `haltransport`. Playback, capture, monitoring and rate switching remained unchanged.

## Checkpoint 2 — shared CMP/ISO lifecycle: validated

`tools/transport/full_duplex_lifecycle.h` owns the common FireWire connection lifecycle:

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

After this extraction, both native engines compiled and were hardware-tested through `haltransport`. Capture, monitoring and playback remained clean at both rates and survived rate changes in both directions. Commit `d7b2d25` is the validated checkpoint immediately after wiring the shared lifecycle into the 44.1 engine.

## Checkpoint 3 — shared full-duplex service loop: validated

`tools/transport/full_duplex_runtime.h` now owns the common steady-state service loop. The exact validated ordering is preserved:

```text
run-loop service
 -> capture receive service
 -> rate-specific CoreAudio playback SHM -> PCM service
 -> native TX scheduler service
 -> capture receive service again
 -> capture prefill activation
 -> periodic combined playback/capture diagnostics
```

The ordering is functional, not cosmetic. RX is serviced before and after playback/TX work to minimize the receive-DMA reuse window; changing that order previously made full-duplex capture audibly broken.

The helper deliberately takes rate-specific behavior as strategy/configuration instead of attempting to merge it:

- 44.1 kHz keeps one `pumpPlayback()` per iteration and a 1 ms CoreFoundation run-loop slice;
- 48 kHz keeps backlog-draining playback service and a 0.25 ms run-loop slice;
- each bridge keeps its native AMDTP streamer/scheduler;
- the capture pump keeps its rate-specific completion-token semantics;
- the 44.1 post-start AV/C reassert remains outside and before the shared steady-state runtime;
- the existing 48 kHz QoS request remains rate-specific startup behavior.

Implementation commits:

```text
bb5a891  Extract common full-duplex service loop
2bf5227  Use shared full-duplex runtime at 48 kHz
6633205  Use shared full-duplex runtime at 44.1 kHz
```

Hardware validation after these commits confirmed no regression. Both native rates compiled and were exercised through `haltransport`; playback, capture and monitoring remained clean, and switching 44.1 -> 48 -> 44.1 continued to work as before.

This is checkpoint 3's known-good hardware baseline.

## Refactoring state after checkpoint 3

The two native bridge binaries now mostly describe rate strategy and startup behavior rather than duplicating the host plumbing, FireWire lifecycle and steady-state service loop.

Common layers:

```text
full_duplex_shared.h
    host SHM / PCM plumbing

full_duplex_lifecycle.h
    CMP / ISO setup and teardown

full_duplex_runtime.h
    steady-state RX -> playback -> TX -> RX service loop
```

Still intentionally rate-specific:

```text
44.1 kHz
    native TX ring/scheduler
    2048-cycle lead
    timestamp-only capture completion
    one-pass playback pump
    1 ms runtime slice
    post-start AV/C rate reassert

48 kHz
    native TX ring/scheduler
    256-cycle lead
    timestamp + isoHeader capture completion
    playback backlog drain
    0.25 ms runtime slice
    user-interactive QoS request
```

Further consolidation should only remove duplication that is genuinely policy-neutral. The validated rate-specific timing and packet behavior should not be abstracted away merely to make the bridge files look symmetrical.

## Next structural step

The next useful consolidation target is the duplicated engine setup surrounding the already-shared layers: opening/validating the HAL rings, discovering/opening the operational FW410, reading initial cycle time, constructing the PCM/RX/TX objects, and entering the shared lifecycle/runtime.

That extraction should be designed as an engine shell with explicit rate strategy hooks. It must not absorb the 44.1 FCP reassertion or merge the two native AMDTP schedulers.

After that structural work is validated, the project should move from refactoring toward runtime robustness: automatic bootloader handling, bus-reset/generation recovery, disconnect/reconnect recovery and a runtime HAL build identifier. Capture-prefill/monitoring-latency tuning remains deliberately later work.