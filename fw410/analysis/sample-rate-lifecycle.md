# FW410 sample-rate lifecycle

This note records the hardware-validated native-rate policy used by `haltransport`, `halbridge44100`, `halbridge48000`, and `rateprobe`.

## Supported native rates

The current CoreAudio/transport path supports native 44.1 kHz and 48 kHz operation. The transport does not sample-rate-convert between them; the supervisor selects the matching native engine.

## Initial rate setup

`rateprobe` first reads both FW410 AV/C signal-format directions:

- device OUTPUT / host capture;
- device INPUT / host playback.

If both directions already match the requested rate, initial setup is now idempotent: no redundant AV/C CONTROL command is sent. If either direction differs, both directions are set and authoritative STATUS readback verifies the transition.

This applies uniformly to 44.1 and 48 kHz.

## 44.1 kHz startup quirk

The FW410 still requires its experimentally established post-start 44.1 kHz reassertion. Skipping a redundant *initial* CONTROL when the hardware is already at 44.1 does not remove this requirement.

Validated 44.1 startup sequence:

```text
read current rate
 -> if needed, set both directions to 44100 and verify
 -> establish duplex CMP/ISO
 -> begin native 44.1 kHz AMDTP traffic
 -> reassert 44100 on OUTPUT plug 0 and INPUT plug 0
 -> native engine reports READY
```

When the device is already at 44.1 kHz, the first transition becomes:

```text
read current rate = 44100/44100
 -> no initial AV/C rate CONTROL
 -> establish duplex CMP/ISO
 -> post-start 44100 reassert
 -> READY
```

## Clean 44.1 kHz shutdown

A clean 44.1 kHz supervisor stop leaves the operational FW410 at 44.1 kHz. The earlier unconditional restore to 48 kHz was retained from development testing and caused an unnecessary 44.1 -> 48 -> 44.1 cycle across ordinary restarts.

Repeated hardware tests after removing that clean-stop restore showed the expected supervisor sequence on each restart:

```text
OFFLINE
 -> RECOVERING [one native-engine PID]
 -> ONLINE [same PID]
```

Repeated runs started with both device directions already at 44.1 kHz, reached READY without failed child chains or bootloader detours, and maintained clean capture. Representative steady-state capture remained near 88,200 frames per two-second interval, with the shared capture cushion around 4k frames and zero drops, malformed packets, DBC gaps, reorders, or stale groups in the repeated validation runs.

## Failure cleanup

The clean-stop policy is deliberately distinct from abnormal engine/runtime failure handling. A failed 44.1 kHz engine retains the existing best-effort 48 kHz restore as a conservative recovery path. Physical disconnect/reconnect remains owned by `haltransport` recovery and guarded boot/re-enumeration logic.

## Rationale

The resulting policy is:

```text
requested rate already active
 -> do not rewrite the same rate before ISO startup

real rate transition required
 -> set both AV/C directions and verify by STATUS

44.1 kHz ISO startup
 -> always perform the proven post-start 44.1 reassert

clean 44.1 kHz stop
 -> leave hardware at 44.1

44.1 kHz engine failure
 -> retain best-effort 48 kHz recovery restore
```

This makes normal startup/shutdown idempotent while preserving the device-specific 44.1 kHz streaming ritual and the conservative error-recovery path.