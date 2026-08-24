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

`tools/transport/full_duplex_shared.h` owns rate-independent host-side plumbing: playback shared-memory mapping/lifetime, backlog discard on engine start, channel permutation, Float32 -> signed 24-bit PCM conversion, common PCM/ring geometry, playback pumping/draining helpers and FireWire cycle-count extraction.

Both native engines were rebuilt after this extraction and hardware-tested through `haltransport`. Playback, capture, monitoring and rate switching remained unchanged.

## Checkpoint 2 — shared CMP/ISO lifecycle: validated

`tools/transport/full_duplex_lifecycle.h` owns the common FireWire connection lifecycle: PCR readiness, device-to-host/host-to-device isoch allocations, port binding, callback/isoch dispatchers and notifications, allocation, CMP connection, playback-first/capture-second start ordering, and staged shutdown/restoration.

The 44.1 kHz bridge still performs its FCP response-space cleanup between ISO/CMP teardown and dispatcher removal, preserving the known-good AV/C lifecycle.

Both rates were hardware-tested after this extraction with no regression.

## Checkpoint 3 — shared full-duplex service loop: validated

`tools/transport/full_duplex_runtime.h` owns the common steady-state ordering:

```text
run-loop service
 -> capture receive service
 -> rate-specific playback SHM -> PCM service
 -> native TX scheduler service
 -> capture receive service again
 -> capture prefill activation
 -> periodic combined diagnostics
```

The rate-specific policies remain explicit: 44.1 uses one playback pump and a 1 ms slice; 48 kHz drains backlog and uses a 0.25 ms slice. Each keeps its native scheduler, capture-token semantics and startup behavior.

Implementation commits:

```text
bb5a891  Extract common full-duplex service loop
2bf5227  Use shared full-duplex runtime at 48 kHz
6633205  Use shared full-duplex runtime at 44.1 kHz
```

Hardware validation confirmed playback, capture, monitoring and 44.1 -> 48 -> 44.1 switching remained clean.

## Checkpoint 4 — common engine setup: validated

`tools/transport/full_duplex_engine_setup.h` now owns the remaining policy-neutral pre-stream setup shared by both native engines:

- open and validate the HAL playback shared ring at the requested native rate;
- discard stale playback backlog;
- open/reinitialize the persistent capture shared ring at the same native rate;
- discover and open the operational `FW 410` FireWire unit;
- obtain the current FireWire cycle time;
- derive `initialCycle` and the rate-specific `firstCycle` using the caller-provided cycle lead.

The native TX ring, native AMDTP streamer, capture-pump configuration, CMP/ISO lifecycle, runtime playback policy, QoS and 44.1 FCP reassert remain outside this helper.

Implementation commits:

```text
8dd0a2a  Extract common full-duplex engine setup
ecd18a9  Use common engine setup at 48 kHz
3bad93a  Use common engine setup at 44.1 kHz
438f4f8  Fix engine setup capture include path
```

After correcting the include path, both bridges compiled and were tested through `haltransport`. Playback, capture and monitoring remained functional at both native rates and rate switching remained functional. Checkpoint 4 is therefore hardware-validated.

### Non-blocking first-start observation

One test began directly at 44.1 kHz with broken audio; switching to 48 kHz and back to 44.1 restored clean operation. This is recorded as an intermittent initialization/state observation rather than a refactor regression because the validated steady-state paths and subsequent bidirectional rate switching remained correct. If it persists, it should be isolated later by comparing a cold 44.1 start, a 44.1 stop/restart without changing rate, and a 44.1 -> 48 -> 44.1 transition.

## Refactoring state after checkpoint 4

The native bridge files now primarily express genuinely rate-specific strategy. Common layers are:

```text
full_duplex_shared.h
    host SHM / PCM plumbing

full_duplex_engine_setup.h
    HAL/capture SHM + operational-device + initial-cycle setup

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

Further structural consolidation is no longer a priority unless it clearly enables robustness or maintainability without obscuring these validated differences.

## Next phase — runtime robustness

The project now moves from refactoring into operational lifecycle work. Priorities are:

1. automatic bootloader detection and guarded boot-to-operational transition in the long-running supervisor;
2. operational-unit reacquisition after the resulting FireWire re-enumeration;
3. bus-reset/generation-change recovery while a native engine is running;
4. disconnect/reconnect recovery;
5. runtime HAL build identification;
6. only later, capture-prefill/monitoring-latency tuning.

The supervisor should orchestrate these transitions. The HAL real-time callback must remain isolated from FireWire discovery, boot and recovery.