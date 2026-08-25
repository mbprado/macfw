# FW410 transport recovery validation

Last updated: 2026-08-25

This note records hardware validation of the `haltransport` recovery state machine, the versioned transport-status ABI, the persistent CoreAudio endpoint behavior, and the launchd-managed transport runtime after the native 44.1/48 kHz full-duplex paths and common transport refactors were stable.

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
2. The supervisor backed off through absent and unstable operational-device states.
3. The FW410 later appeared in bootloader mode and passed the guarded preflight.
4. Exactly one boot cue was issued.
5. Additional transient attempts were handled by guard refusal / candidate-unavailable outcomes rather than unsafe writes.
6. The operational FW410 eventually reappeared at its default device state.
7. `halbridge44100` selected 44.1 kHz, started duplex ISO and performed the required post-start AV/C reassert.
8. Both OUTPUT plug 0 and INPUT plug 0 accepted 44.1 kHz.
9. Playback, recording and software monitoring resumed cleanly without manually switching through 48 kHz.

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

## 44.1 clean-stop rate policy — validated

Earlier test code restored the FW410 to 48 kHz at the end of every clean 44.1 kHz engine run. Repeated hardware testing showed that this forced `44.1 -> 48 -> 44.1` lifecycle materially increased the frequency of broken or slow 44.1 startup states.

The clean-stop policy was changed so a normal 44.1 kHz stop leaves the device at 44.1 kHz. Abnormal/error shutdown retains the historical best-effort 48 kHz recovery behavior.

Repeated stop/start testing then produced the intended sequence consistently:

```text
ONLINE
    -> clean stop
    -> FW410 remains at 44100 Hz
    -> supervisor restart
    -> initial rate setup sees 44100/44100 already selected
    -> no redundant initial AV/C rate CONTROL
    -> duplex ISO starts
    -> required post-start 44.1 AV/C reassert
    -> READY
    -> ONLINE
```

After this change, the previously intermittent broken-audio occurrence reduced significantly to practically zero in subsequent testing. This is now the standard 44.1 kHz lifecycle policy rather than an experiment.

## Historical 44.1 capture degradation observation

Before the clean-stop lifecycle was standardized, one 44.1 kHz run showed audibly broken recording while capture diagnostics drifted away from the validated steady state:

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

Keep this as historical evidence for the rate-lifecycle investigation. Subsequent testing after removing the unconditional clean-stop restore to 48 kHz reduced this failure mode to practically zero.

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

The status object is intentionally persistent across `haltransport` process restarts. This is required because `coreaudiod` may keep its mapping for the lifetime of the loaded HAL plug-in. Unlinking and recreating the object would leave the HAL attached to an orphaned old object while a restarted supervisor wrote status into a new object with the same name.

Validated lifecycle:

```text
haltransport ONLINE
    -> stop/restart supervisor
    -> existing shared object published OFFLINE
    -> Logic/CoreAudio keeps running
    -> restarted supervisor reopens same object
    -> RECOVERING
    -> native engine READY
    -> ONLINE
    -> playback and real capture resume without reboot
```

On Darwin the backing object may report a page-sized `st_size` (for example 4096 bytes) even though the ABI structure is 48 bytes. Compatibility therefore requires the backing object to be **at least** `sizeof(SharedStatus)`; the mapped structure's `magic`, `version`, and `structSize` remain the actual ABI validation.

### Startup-order independence — validated

The HAL now establishes the persistent status object during non-real-time initialization if no supervisor has created it yet. The initial placeholder is `OFFLINE` with zero requested/active rate and zero engine PID. `haltransport` later reopens the same object and becomes the state publisher.

Validated startup sequence:

```text
coreaudiod / HAL starts first
    -> status object exists as OFFLINE
    -> haltransport starts later
    -> RECOVERING
    -> native engine READY
    -> ONLINE
```

This removes the previous startup-order dependency without putting `shm_open`, `mmap`, allocation, or retry work into a real-time audio callback.

### Explicit native-engine READY handshake

`ONLINE` does not mean merely that a child process exists. Each native engine explicitly signals READY to its parent only after reaching its proven operational point:

- **48 kHz:** after successful duplex ISO startup;
- **44.1 kHz:** after duplex ISO startup and successful post-start AV/C 44.1 kHz reassertion.

Until READY arrives, a launched child remains `RECOVERING`.

## Persistent CoreAudio endpoint and offline audio behavior — validated

The HAL consumes the transport-status ABI and keeps the logical FW410 device registered regardless of physical transport state.

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
- stopping/restarting `haltransport` causes the same safe offline/recovery behavior without reboot;
- physical disconnect/reconnect followed by later supervisor restart also works without reboot.

This establishes the intended architectural separation: the logical CoreAudio device lifetime is independent of both the physical FW410 connection and the `haltransport` process lifetime.

## launchd-managed runtime — validated

`haltransport` is now deployable as the system launchd service:

```text
com.mbprado.macfw.fw410.transport
```

The development installer places a self-contained runtime under:

```text
/Library/Application Support/macfw/fw410
```

and installs the corresponding LaunchDaemon plist under `/Library/LaunchDaemons`.

The runtime tree includes the supervisor, both native-rate bridges, `fwboot`, `rateprobe`, `transportstatus`, and `deviceprobe`. The initial service test exposed a missing packaged `rateprobe` dependency; after adding it to the runtime tree, the launchd path reached stable `ONLINE` operation.

Validated service lifecycle:

- service install/uninstall scripts work;
- install is gated by `deviceprobe --require-supported`;
- operational and bootloader FW410 personalities are accepted by the hardware gate;
- launchd starts `haltransport` without a Terminal session;
- 44.1 and 48 kHz playback/capture continue to work under launchd ownership;
- changing 44.1 <-> 48 kHz works under launchd ownership;
- physical disconnect/reconnect keeps the CoreAudio endpoint alive, produces silence while offline, and resumes audio after recovery;
- reboot with the FW410 connected automatically restores transport/audio without manually launching `haltransport`;
- reboot with the FW410 physically absent also works: launchd starts the supervisor/runtime without hardware present, and connecting the interface only after macOS is fully booted is handled automatically until transport reaches normal operation;
- forcibly killing the launchd-owned supervisor causes launchd to restart it automatically.

Observed forced-restart evidence:

```text
runs = 2, pid = 285
    -> kill 285
runs = 3, pid = 688
    -> kill 688
runs = 4, pid = 703
```

In each case launchd returned the service to `state = running` with a new supervisor PID. This validates process supervision independently from the supervisor's own FireWire recovery state machine.

With delayed post-boot hardware attachment now validated, the launchd service-runtime lifecycle is considered complete for the first alpha packaging pass.

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
| HAL status object startup-order independence | validated | validated |
| HAL offline playback discard | validated | validated |
| HAL offline capture silence | validated | validated |
| active Logic recording survives outage | validated | validated |
| `haltransport` restart without reboot | validated | validated |
| launchd automatic supervisor restart | validated | validated |
| reboot with interface connected | validated | validated service architecture |
| reboot without interface + delayed attach | validated | validated service architecture |
| launchd rate switching | validated | validated |
| launchd physical reconnect recovery | validated | validated |
| 44.1 post-start AV/C reassert after reconnect | validated | n/a |

## CoreAudio availability policy

Do **not** make physical FireWire disconnects, supervisor restarts, or launchd restarts remove/recreate the CoreAudio endpoint. The validated policy is:

```text
CoreAudio endpoint remains registered
        ↓
transport state becomes RECOVERING / OFFLINE
        ↓
HAL safely discards playback and supplies capture silence
        ↓
FireWire supervisor / launchd recovers runtime
        ↓
transport state becomes ONLINE
        ↓
existing CoreAudio clients continue using the same device instance
```
