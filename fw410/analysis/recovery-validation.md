# FW410 transport recovery validation

Last updated: 2026-08-25

This note records the hardware validation of the `haltransport` recovery state machine after the native 44.1/48 kHz full-duplex paths and common transport refactors were already stable.

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
    -> full-duplex service resumes
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
| 44.1 post-start AV/C reassert after reconnect | validated | n/a |

## CoreAudio availability policy

During transport outages the logical CoreAudio device currently remains registered with macOS and remains visible to applications such as Logic. This is the intended architectural direction.

Do **not** make physical FireWire disconnects remove/recreate the CoreAudio endpoint. The target policy is:

```text
CoreAudio endpoint remains registered
        ↓
transport state becomes RECOVERING / OFFLINE
        ↓
HAL safely discards playback and supplies silence/empty capture
        ↓
FireWire supervisor recovers the FW410
        ↓
transport state becomes ONLINE
        ↓
existing CoreAudio clients continue using the same device instance
```

The next implementation step is a small versioned transport-status shared-memory ABI between `haltransport` and the HAL. It should communicate transport availability without moving FireWire lifecycle work into CoreAudio real-time callbacks.
