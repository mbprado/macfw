# FW410 current integration status

Last updated: 2026-08-24

This document is the current handoff for the CoreAudio/HAL/runtime phase. Older reverse-engineering documents remain useful historical evidence, but this file defines the present integration state and immediate next work.

## Executive status

macfw publishes **M-Audio FireWire 410** as a normal macOS CoreAudio device through a dependency-free AudioServerPlugIn.

Hardware-confirmed:

- the device appears in Audio MIDI Setup and the normal macOS audio selector;
- native 44.1 kHz and 48 kHz playback work;
- all eight analog outputs and both S/PDIF outputs were independently validated in Logic Pro;
- CoreAudio exposes four input channels: Analog In 1/2 and S/PDIF In L/R;
- native full-duplex playback + capture works at both 44.1 and 48 kHz;
- live software monitoring works at both native rates;
- `haltransport` switches native engines when the CoreAudio rate changes;
- repeated 44.1 <-> 48 kHz switching preserves playback, capture and monitoring;
- four transport refactoring checkpoints have been hardware-validated without regression: shared HAL/PCM plumbing, shared engine setup, shared CMP/ISO lifecycle, and the shared steady-state full-duplex service loop;
- physical FireWire disconnect/reconnect recovery has been hardware-validated at 48 kHz, including bootloader re-entry, guarded boot cue, re-enumeration and automatic full-duplex recovery.

The current architecture is:

```text
macOS application
    -> CoreAudio
    -> macfw AudioServerPlugIn
       -> playback shared ring
       <- capture shared ring
    -> companion user-space transport
       -> common HAL/PCM plumbing
       -> common engine setup
       -> common CMP/ISO lifecycle
       -> common full-duplex service loop
       -> rate-specific AMDTP/timing strategy
    -> AV/C / CMP / CIP / AMDTP / NuDCL / IOFireWireLib
    -> M-Audio FireWire 410
```

The HAL never owns FireWire. Real-time callbacks only move PCM through versioned lock-free shared memory. Device boot, rate control, CMP, ISO, AMDTP scheduling and recovery belong in the transport/service layer.

## Playback status

At 44.1/48 kHz CoreAudio presents ten outputs: Analog Out 1-8 and S/PDIF L/R. The raw FW410 playback order is unusual and is explicitly permuted by the transport; `analysis/stream-topology.md` is the mapping source of truth.

### Native 44.1 kHz

The 44.1 engine is hardware-confirmed and clean. The tested FW410 requires this startup sequence:

1. select 44.1 kHz in both AV/C signal-format directions;
2. establish duplex CMP/ISO;
3. start valid native 44.1 AMDTP traffic;
4. while streaming is live, reassert 44.1 on OUTPUT plug 0 and INPUT plug 0;
5. continue normal data-bearing scheduling.

This startup quirk remains explicitly rate-specific.

One intermittent first-start condition is still tracked separately: on some runs the first direct 44.1 start is audibly broken, while switching to 48 kHz and back to 44.1 restores correct operation. This is currently treated as a startup/state issue, not a steady-state transport regression.

### Native 48 kHz

The 48 kHz engine is hardware-confirmed and clear. The decisive fix for early playback corruption was increasing transmit scheduling margin to a 640-cycle ring / 320-cycle refill half. The committed path also uses a 16,384-frame PCM FIFO, user-interactive transport QoS and late/silence diagnostics.

## Rate-aware full-duplex supervisor and recovery

`tools/transport/haltransport` is the current rate-aware full-duplex runtime supervisor. Both native engines provide ten-channel playback and four-channel capture. The supervisor reads the HAL-selected sample rate, starts the matching native engine and restarts it when the rate changes.

Repeated 44.1 <-> 48 kHz switching has been hardware-tested after the current transport refactors with playback, capture and monitoring remaining functional after each transition.

The supervisor now also owns explicit transport-recovery states. A native engine monitors the FireWire bus generation while streaming. If the generation changes or can no longer be read, the engine exits through its normal cleanup path so the supervisor can reacquire the device rather than continuing against stale generation/node/DMA state.

Recovery behavior includes:

```text
RUNNING
 -> generation change / engine failure
 -> guarded bootloader check
 -> WAIT_REENUMERATION if a guarded boot cue was actually issued
 -> BACKOFF on absent/unstable device
 -> fresh native-engine launch
 -> fresh FW410 generation/node acquisition
 -> new CMP/ISO/DMA lifecycle
 -> RUNNING
```

`fwboot --execute` reports distinct guarded outcomes so `haltransport` no longer treats every helper exit as a successful boot. Retry backoff prevents a tight child-process loop while the interface is absent or the FireWire bus is unstable.

### Physical disconnect/reconnect validation

A live 48 kHz full-duplex test was performed by physically disconnecting and reconnecting the FW410 while `haltransport` remained running.

The running engine detected a FireWire generation change and exited. During reconnection, several attempts saw unstable generation/node state and failed harmlessly under backoff. The FW410 then appeared as the known bootloader personality, passed the guarded preflight and received exactly one boot-from-flash cue. The supervisor waited for re-enumeration, refused a subsequent transitional loader state whose guard no longer matched, then successfully launched the operational 48 kHz engine.

Playback and capture both returned to normal operation automatically after the reconnect. This validates the current generation-change detection, guarded boot, backoff, re-enumeration and operational-unit reacquisition path at 48 kHz.

A future refinement may replace the fixed post-boot wait with explicit detection of a stable operational FW410 before launching the native engine. The current behavior is safe and functionally validated, but one harmless transitional engine attempt can still occur before the operational personality is fully ready.

## CoreAudio transport-offline policy

The logical CoreAudio device currently remains visible in macOS/Logic even when the physical FW410 is disconnected. This separation is considered desirable and should be preserved.

The intended future policy is:

```text
CoreAudio device remains registered
        ↓
transport disconnects
        ↓
HAL exposes transport offline/unavailable state
        ↓
playback is safely discarded / rendered as silence
capture returns silence / empty input safely
        ↓
transport supervisor recovers FireWire device
        ↓
HAL transport state returns online
        ↓
existing CoreAudio clients continue without device removal/recreation
```

The goal is transparent recovery without forcing Logic or other applications to lose and reselect the interface. This policy still needs a transport-status ABI between the supervisor/transport and the HAL; it is an architectural decision, not yet a completed HAL feature.

## Capture topology and validated receive model

The incoming FW410 AMDTP stream is DBS=5:

```text
raw position 1  S/PDIF In L
raw position 2  Analog In 1
raw position 3  S/PDIF In R
raw position 4  Analog In 2
raw position 5  MIDI
```

CoreAudio presents Analog In 1/2 and S/PDIF In L/R. The capture ABI is a separate four-channel Float32 shared ring defined in `hal/include/macfw_hal_capture_shm.h`.

The validated receive path is:

```text
FW410 input
 -> device-to-host ISO
 -> NuDCL receive ring
 -> 32-cycle metadata publication groups
 -> terminal-slot-confirmed completed group
 -> AMDTP/CIP/DBC validation
 -> AM824 MBLA-24 decode
 -> four-channel Float32 capture ring
 -> AudioServerPlugIn ReadInput
 -> CoreAudio application
```

Every group contains 32 receive DCL slots. The terminal receive DCL publishes the group's metadata update list, and userspace snapshots only that completed group. DBC continuity is validated/ordered inside the group.

The completion token is intentionally rate-specific:

```text
48 kHz: terminal timestamp + isoHeader
44.1 kHz: terminal timestamp only
```

At 44.1 kHz the alternating blocking/NODATA pattern makes the ISO header unsuitable as part of the completed-group generation token.

The producer uses a 4,096-frame prefill before enabling the CoreAudio consumer. This is ~85 ms at 48 kHz and ~93 ms at 44.1 kHz. It is intentionally conservative; monitoring-latency tuning is deferred until runtime robustness work is complete.

Detailed capture design and evidence remain in `analysis/capture-pipeline.md`.

## Full-duplex service ordering

The first combined 48 kHz full-duplex version regressed capture quality even though packet diagnostics remained clean. The cause was service ordering: playback work ran before receive consumption while RX slots were continuously reused by DMA.

The validated runtime therefore preserves:

```text
CoreFoundation run-loop service
 -> capture service
 -> playback SHM/PCM service
 -> native TX scheduler
 -> capture service again
 -> capture prefill activation
 -> periodic diagnostics
```

This ordering is now shared by both native engines through `tools/transport/full_duplex_runtime.h`.

Rate-specific policies remain explicit: 44.1 uses one playback pump per loop with a 1 ms slice, while 48 kHz drains playback backlog with a 0.25 ms slice. Each rate retains its own native AMDTP scheduler and capture completion semantics.

## Transport refactoring checkpoints

Four incremental structural checkpoints are now hardware-validated.

### Checkpoint 1 — host HAL/PCM plumbing

`tools/transport/full_duplex_shared.h` owns shared-memory mapping, backlog handling, channel permutation, Float32-to-24-bit conversion, PCM geometry and cycle-count helpers.

### Checkpoint 2 — CMP/ISO lifecycle

`tools/transport/full_duplex_lifecycle.h` owns the common operational-device connection lifecycle: ISO allocation, PCR connection, dispatcher/notification setup, start ordering and teardown. The 44.1 FCP response-space cleanup remains staged around this helper to preserve its proven startup/shutdown behavior.

### Checkpoint 3 — steady-state service loop

`tools/transport/full_duplex_runtime.h` owns the common RX -> playback -> TX -> RX runtime structure, prefill activation and combined diagnostics while accepting rate-specific playback and timing policy.

### Checkpoint 4 — common engine setup

`tools/transport/full_duplex_engine_setup.h` owns the policy-neutral pre-stream setup: HAL playback/capture SHM validation, operational FW410 discovery/opening, initial cycle acquisition and rate-specific first-cycle derivation. Native scheduler construction, capture-token behavior, QoS and the 44.1 FCP reassert remain outside it.

After these changes both engines were compiled and tested through `haltransport`. Playback, capture and monitoring remained clean at 44.1 and 48 kHz, including rate switching. No refactor regression was observed.

See `analysis/transport-refactoring.md` for the detailed boundary between shared mechanisms and rate-specific strategy.

## HAL reload caveat

During development, rebuilding/installing the AudioServerPlugIn and restarting `coreaudiod` did not always guarantee that the newly installed HAL binary was active. A full reboot was sometimes required before behavior matched the newly built code.

This remains a separate lifecycle/debugging issue. Future instrumentation should expose an explicit runtime HAL build/version identifier so tests can prove which binary `coreaudiod` is executing.

## Bootloader / interface boot mode

Production startup supports the two observed personalities at the supervisor level:

```text
FW Bootloader  model 0x00010058
FW 410         model 0x00010046
```

The guarded bootloader -> operational transition is now integrated with `haltransport`. The supervisor can recover from a reconnect into bootloader mode, wait/retry through re-enumeration and launch a fresh native engine after the operational unit becomes available.

Do not put this lifecycle work in a HAL real-time callback.

## Current functional matrix

| Capability | 44.1 kHz | 48 kHz |
|---|---|---|
| 10-channel playback | validated | validated |
| 4-channel capture | validated | validated |
| simultaneous full duplex | validated | validated |
| software monitoring | validated | validated |
| `haltransport` launch | validated | validated |
| runtime rate switching | validated | validated |
| shared transport refactor | validated | validated |
| generation-change detection | implemented | implemented |
| physical disconnect/reconnect recovery | not yet explicitly tested | validated |
| guarded bootloader recovery | implemented | validated during reconnect |

## Immediate sequence

1. Validate physical disconnect/reconnect recovery at 44.1 kHz as well, including its post-start AV/C reassertion after recovery.
2. Add a small transport-status ABI shared with the HAL so the CoreAudio endpoint can explicitly report online/offline state while remaining registered.
3. Add a runtime HAL build identifier to eliminate stale-load ambiguity during development.
4. Improve post-boot operational readiness detection so `haltransport` waits for a stable operational unit instead of relying primarily on a fixed re-enumeration delay.
5. Tune capture prefill / monitoring latency after runtime recovery is reliable.
6. Return to mixer/routing/headphone, S/PDIF and MIDI integration.
7. Finish packaging/release automation when the runtime is reproducibly installable.

## Quick handoff

> Native 44.1 and 48 kHz full duplex are hardware-validated through `haltransport`: ten-channel playback, four-channel capture and software monitoring work at both rates, and repeated native rate switching works. The transport refactor is complete enough that shared HAL/PCM plumbing, engine setup, CMP/ISO lifecycle and the steady-state service loop are common while rate-specific AMDTP behavior remains explicit. Physical disconnect/reconnect recovery has now been hardware-validated at 48 kHz: the running engine detected a generation change, the supervisor backed off through unstable FireWire state, issued one guarded boot cue when the known loader appeared, reacquired the operational FW410 and restored playback/capture automatically. The CoreAudio device should remain logically registered across transport outages; future HAL work should expose transport offline/online state and provide silence safely while disconnected rather than removing/recreating the device.