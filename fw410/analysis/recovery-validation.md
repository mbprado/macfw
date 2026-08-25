# FW410 transport recovery validation

Last updated: 2026-08-25

This note records hardware validation of the `haltransport` recovery state machine, the versioned transport-status ABI, and the persistent CoreAudio endpoint behavior after the native 44.1/48 kHz full-duplex paths and common transport refactors were already stable.

## Recovery architecture under test

The supervisor/runtime recovery chain is:

```text
native full-duplex engine
    -> streaming-time FireWire generation monitoring
    -> controlled exit on generation change / stale bus state
    -> haltransport supervisor
    -> guarded FW410 bootloader check
    -> explicit WAIT_REENUMERATION / BACKOFF states
    -> fresh native-engine launch
    -> fresh generation/node acquisition
    -> rate setup
    -> fresh CMP / ISO / DMA lifecycle
    -> native engine READY signal
    -> supervisor publishes ONLINE
    -> full-duplex service continues
```

`fwboot --execute` uses explicit guarded result codes, so the supervisor can distinguish a boot cue that was actually issued from no-loader, guard-refused and unstable-candidate outcomes. Retry delay backs off instead of spinning when the device is absent or the FireWire bus is unstable.

## 48 kHz physical disconnect/reconnect — validated

A live 48 kHz full-duplex session was physically disconnected and reconnected while `haltransport` remained running.

Observed sequence:

1. The running engine detected a FireWire generation change and exited through normal cleanup.
2. Early retries observed unstable generation/node state and failed harmlessly.
3. The FW410 reappeared in its known bootloader personality.
4. Guarded `fwboot` preflight passed and one boot-from-flash cue was issued.
5. Transitional loader state was subsequently refused by the guard rather than written again.
6. The operational FW410 reappeared and a fresh 48 kHz engine started.
7. Playback and capture both resumed automatically.

After recovery, capture returned to native 48 kHz cadence with an approximately 4k-frame capture cushion and no sustained capture drops or malformed packets.

## 44.1 kHz physical disconnect/reconnect — validated

The same test was then performed while the device was already running cleanly at native 44.1 kHz.

Before disconnect, representative capture status was healthy:

```text
capture delta = 88200 frames / ~2 s
queued ~= 4k frames
in-drops = 0
malformed = 0
invalid = 0
dbc-gap = 0
reorder = 0
stale = 0
```

On physical disconnect:

1. The running 44.1 engine detected the FireWire generation change.
2. Its best-effort 48 kHz shutdown restore could not complete against the disconnected/stale generation; this did not block supervisor recovery.
3. The supervisor backed off through absent and unstable operational-device states.
4. The FW410 later appeared in bootloader mode and passed the guarded preflight.
5. Exactly one boot cue was issued.
6. Additional transient attempts were handled by guard refusal / candidate-unavailable outcomes rather than unsafe writes.
7. The operational FW410 eventually reappeared at 48 kHz default device state.
8. `halbridge44100` selected 44.1 kHz, started duplex ISO and performed the required post-start AV/C reassert.
9. Both OUTPUT plug 0 and INPUT plug 0 accepted 44.1 kHz.
10. Playback, recording and software monitoring resumed cleanly without manually switching through 48 kHz.

Representative recovered steady-state capture again returned to:

```text
capture delta = 88200 frames / ~2 s
queued ~= 4k frames
in-drops = 0
malformed = 0
invalid = 0
dbc-gap = 0
reorder = 0
stale = 0
```

This proves that automatic reconnect recovery is not dependent on a manual 48 -> 44.1 rate transition. The supervisor can recover directly back into the requested native 44.1 kHz full-duplex state.

## 48 -> 44.1 transition observation

A separate test began with the interface physically connected and working at 48 kHz, then changed the CoreAudio rate to 44.1 kHz. The transition eventually succeeded and produced clean 44.1 playback/capture, but it passed through several unstable FireWire generations, failed rate-control/reassert attempts and bootloader cycles before settling.

This is consistent with the previously observed intermittent 44.1 first-start/state anomaly. It is not considered a steady-state transport regression because the final 44.1 state is clean and repeatable once established. Keep this transition log as evidence when the 44.1 startup anomaly is investigated later.

## 44.1 capture degradation observation

A later 44.1 kHz run showed a different but likely related intermittent state problem. Playback continued, but the recording became audibly broken while capture diagnostics drifted away from the validated steady state:

```text
dbc-gap: 70 -> 144 -> 294 -> ... -> 1202
queued:  ~3880 -> 3464 -> 2880 -> ... -> ~200
capture delta: roughly 87896-88080 frames / ~2 s
```

CoreAudio continued requesting input at the expected cadence. Stopping `haltransport` and starting it again, without rebooting or restarting Logic, immediately restored clean 44.1 capture:

```text
capture delta ~= 88200 frames / ~2 s
queued ~= 4k frames
in-drops = 0
malformed = 0
invalid = 0
dbc-gap = 0
reorder = 0
stale = 0
```

This episode is recorded as another manifestation of the intermittent 44.1 initialization/state anomaly, not as a persistent-SHM or CoreAudio-offline regression. The fact that a transport-process restart alone clears it is useful evidence for later investigation.

## Transport-status ABI — implemented and hardware-validated

`haltransport` publishes a small versioned POSIX shared-memory status block defined by `hal/include/macfw_hal_transport_status.h`. The current ABI exposes:

- `OFFLINE`, `RECOVERING`, and `ONLINE` transport states;
- requested native rate;
- active native-engine rate;
- current native-engine PID;
- transition sequence;
- heartbeat sequence.

`tools/transport/transportstatus` is the diagnostic reader and supports one-shot and `--watch` modes.

The Darwin POSIX SHM object uses the deliberately short name `/macfw_fw410_status_v1`.

### Persistent object identity across supervisor restarts

The status object is now intentionally persistent across `haltransport` process restarts. This is required because `coreaudiod` may keep its mapping for the lifetime of the loaded HAL plug-in. Unlinking and recreating the object would leave the HAL attached to an orphaned old object while a restarted supervisor wrote status into a new object with the same name.

Validated lifecycle:

```text
haltransport ONLINE
    -> Ctrl-C
    -> existing shared object published OFFLINE
    -> Logic/CoreAudio keeps running
    -> restart same haltransport binary
    -> same shared object reopened in place
    -> RECOVERING
    -> native engine READY
    -> ONLINE
    -> playback and real capture resume without reboot
```

On Darwin the backing object may report a page-sized `st_size` (for example 4096 bytes) even though the ABI structure is 48 bytes. Compatibility therefore requires the backing object to be **at least** `sizeof(SharedStatus)`; the mapped structure's `magic`, `version`, and `structSize` remain the actual ABI validation.

### Explicit native-engine READY handshake

`ONLINE` no longer means merely that a child process exists or survived an arbitrary grace period. Each native engine explicitly signals READY to its parent only after reaching its proven operational point:

- **48 kHz:** after successful duplex ISO startup;
- **44.1 kHz:** after duplex ISO startup and successful post-start AV/C 44.1 kHz reassertion.

Until READY arrives, a launched child remains `RECOVERING`. The older survival interval remains useful only for recovery-backoff stabilization; it is no longer the definition of transport readiness.

Hardware validation showed the intended distinction clearly. During physical reconnect, several temporary child PIDs were launched while the FireWire bus/device was still transitioning. They appeared as `RECOVERING` and disappeared back to `active rate = 0 / pid = 0` when those attempts failed. Only the final stable child published READY and caused the supervisor to transition to `ONLINE`.

Representative successful end of a reconnect sequence:

```text
transport state: RECOVERING
    requested rate: 48000 Hz
    active rate:    48000 Hz
    engine pid:     1101

transport state: ONLINE
    requested rate: 48000 Hz
    active rate:    48000 Hz
    engine pid:     1101
```

A normal 44.1 -> 48 kHz rate switch also showed `RECOVERING -> ONLINE` with the same new child PID, confirming that readiness is tied to the engine handshake rather than a fixed delay.

## Persistent CoreAudio endpoint and offline audio behavior — validated

The HAL now consumes the transport-status ABI and keeps the logical FW410 device registered regardless of physical transport state.

Validated policy:

```text
ONLINE
    playback -> normal shared PCM ring
    capture  -> normal capture ring

RECOVERING / OFFLINE
    playback -> accepted by CoreAudio but discarded
    capture  -> zero-filled silence
    device   -> remains registered/alive in CoreAudio
```

Hardware/application validation in Logic confirmed:

- physically disconnecting the FW410 does not make Logic lose the selected device;
- Logic continues its playback timeline while the physical interface is absent;
- active recording continues and records silence during the outage;
- when the FW410 reconnects and recovery reaches `ONLINE`, playback resumes automatically;
- the same recording continues with real input again after recovery;
- no Logic restart or device reselection is required;
- stopping `haltransport` causes the same safe offline behavior;
- restarting `haltransport` now restores playback/capture without rebooting because the status SHM object identity remains stable;
- physical disconnect/reconnect followed by later `haltransport` stop/restart also works without reboot.

This establishes the intended architectural separation: the logical CoreAudio device lifetime is independent of both the physical FW410 connection and the `haltransport` process lifetime.

## Validated recovery matrix

| Recovery behavior | 44.1 kHz | 48 kHz |
|---|---|---|
| generation-change detection | validated | validated |
| physical disconnect detection | validated | validated |
| retry/backoff while absent | validated | validated |
| guarded bootloader handling | validated | validated |
| bootloader -> operational reacquisition | validated | validated |
| native-rate restoration | validated | validated |
| full-duplex playback recovery | validated | validated |
| capture recovery | validated | validated |
| software-monitoring recovery | validated | validated |
| explicit engine READY | validated | validated |
| status ABI rate-change reporting | validated | validated |
| status ABI reconnect reporting | validated | validated |
| persistent status object across supervisor restart | validated | validated |
| HAL offline playback discard | validated | validated architecture |
| HAL offline capture silence | validated | validated architecture |
| active Logic recording survives outage | validated | validated architecture |
| `haltransport` restart without reboot | validated | validated architecture |
| 44.1 post-start AV/C reassert after reconnect | validated | n/a |

## CoreAudio availability policy

Do **not** make physical FireWire disconnects or supervisor restarts remove/recreate the CoreAudio endpoint. The validated policy is:

```text
CoreAudio endpoint remains registered
        ↓
transport state becomes RECOVERING / OFFLINE
        ↓
HAL safely discards playback and supplies capture silence
        ↓
FireWire supervisor recovers or restarts
        ↓
transport state becomes ONLINE
        ↓
existing CoreAudio clients continue using the same device instance
```

The remaining startup-order problem is narrower: if `coreaudiod` loads the HAL before the transport-status object has ever been created, the HAL currently has no non-real-time mechanism to attach later. The next checkpoint is to make late attachment/re-attachment safe without putting `shm_open`, `mmap`, logging, or allocation in a real-time audio callback.
