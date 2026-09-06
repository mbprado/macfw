# FW410 sample-rate lifecycle

Last updated: 2026-09-06

This note records the hardware-validated native-rate policy used by `haltransport`, `halbridge44100`, `halbridge48000`, the HAL sample-rate property and `rateprobe`.

## Supported native rates

The current CoreAudio/transport path supports native 44.1 kHz and 48 kHz operation. The transport does not sample-rate-convert between them; the supervisor selects the matching native engine.

The native AppKit Device tab changes rate by setting `kAudioDevicePropertyNominalSampleRate`. The HAL then requests the normal CoreAudio device configuration change. The GUI does not call `rateprobe` directly and never opens FireWire.

## Initial rate setup

`rateprobe` reads both FW410 AV/C signal-format directions:

- device OUTPUT / host capture;
- device INPUT / host playback.

If both directions already match the requested rate, setup is idempotent: no redundant initial AV/C CONTROL command is sent. If either direction differs, both directions are set and authoritative STATUS readback verifies the transition.

This applies to both supported rates.

## 44.1 kHz startup sequence

44.1 kHz retains the experimentally established post-start rate reassertion.

Current sequence:

```text
read current rate
 -> if needed, set both directions to 44100 and verify
 -> establish duplex CMP/ISO
 -> start native 44.1 AMDTP with the rate-specific cycle lead
 -> continuously service RX/TX during the startup interval
 -> reassert 44100 on OUTPUT plug 0
 -> reassert 44100 on INPUT plug 0
 -> expose meter socket
 -> report native-engine READY
```

When the device is already at 44.1 kHz:

```text
read current rate = 44100/44100
 -> no initial AV/C rate CONTROL
 -> establish duplex CMP/ISO
 -> service audio during startup lead
 -> post-start 44100 OUTPUT/INPUT reassert
 -> expose meter socket
 -> READY
```

The startup lead/reassert delay is not an unserviced audio window. RX publication, playback pumping and TX refill continue during this interval so the transport does not deplete its live rings before READY.

The meter endpoint is intentionally withheld until after reassert because the GUI meter client uses a short timeout. Exposing the listener during the longer 44.1 startup interval previously allowed a request to time out before the engine began servicing that socket.

## 48 kHz startup sequence

48 kHz does not require the special 44.1 post-start AV/C reassert.

```text
read current rate
 -> if needed, set both directions to 48000 and verify
 -> establish duplex CMP/ISO
 -> start native 48 kHz AMDTP with its shorter cycle lead
 -> READY
```

## Rate-specific startup lead

The current engines intentionally retain different start leads:

- 44.1 kHz: 2048 FireWire cycles (~256 ms at 8 kHz cycle rate);
- 48 kHz: 256 FireWire cycles (~32 ms).

The 44.1 value is historical and was explicitly restored after earlier refactoring. Because the current 44.1 path is hardware-stable with excellent latency, do not reduce this value casually before release.

## Real-time scheduling after READY

Both native engines use the same validated service architecture:

```text
normal/control thread
 -> normal FireWire callbacks + FCP/control

dedicated isoch callback thread
 -> dedicated CFRunLoop
 -> USER_INTERACTIVE QoS

dedicated audio service thread
 -> 250 us Mach pacing
 -> USER_INTERACTIVE QoS
 -> THREAD_TIME_CONSTRAINT_POLICY
      period      2000 us
      computation  500 us
      constraint  2000 us
 -> capture + playback + TX service + meter accumulation
```

This architecture is hardware-validated at both 44.1 and 48 kHz and is the release baseline.

## Local socket lifetime / SIGPIPE rule

The transport process owns local Unix sockets used by control and meter clients. These clients can legitimately disappear during a sample-rate transition, especially while the old engine is shutting down and the new one is not fully READY yet.

A previously observed failure sequence was:

```text
44.1 engine reaches READY
 -> supervisor starts persistent-state restore
 -> GUI/control client disconnects while transport is preparing a reply
 -> plain send() raises SIGPIPE
 -> native engine exits on signal 13
 -> supervisor enters bootloader check / retry / backoff
```

The release policy is therefore:

- short-lived/disappearing local IPC clients must never be able to terminate the native audio engine with `SIGPIPE`;
- the 44.1 meter listener is not exposed during the pre-reassert startup window;
- IPC failure remains a client/control concern, not an audio-engine fatal condition.

Repeated hardware testing after this change confirmed reliable 48 -> 44.1 transitions with the control panel open and meter polling active.

## Clean shutdown / failure policy

A clean 44.1 kHz stop leaves the operational FW410 at 44.1 kHz. The earlier unconditional clean-stop restore to 48 kHz caused unnecessary ordinary restart transitions and remains removed.

Abnormal 44.1 engine failure keeps the existing best-effort 48 kHz restore as a conservative recovery path. Physical disconnect/reconnect remains owned by `haltransport` recovery and guarded boot/re-enumeration logic.

## Tested forced re-arm experiment

An intermittent bad-sounding foreground startup originally appeared correlated with starting while the interface was already nominally at 44.1 kHz. A manual real 44.1 -> 48 -> 44.1 AV/C transition could clear one observed bad state, so an unconditional startup re-arm was tested in production code.

That forced re-arm did **not** reliably solve the actual symptom and added unnecessary startup delay, so it was reverted.

Subsequent testing showed an important operational distinction:

- normal launchd-managed transport reloads consistently return in the expected good state;
- foreground `MACFW_VERBOSE=1` runs can behave worse.

Verbose diagnostics are not performance-neutral because reporting occurs on the real-time audio service path. Therefore normal launchd service operation is the release reference.

## 48 -> 44.1 switching is slower but reliable

The asymmetry is expected from the current sequence.

A real 48 -> 44.1 switch includes:

1. AV/C change to 44.1 plus the existing `rateprobe` settle/readback interval (currently 350 ms when a real change is sent);
2. the 2048-cycle 44.1 ISO start lead (~256 ms);
3. two synchronous post-start 44.1 FCP reassert transactions before READY.

By comparison, 44.1 -> 48 uses the shorter 48 kHz start lead and no post-start reassert.

The slow direction is now consistently reliable from both the native control panel and Audio MIDI Setup, and repeated switching did not affect any other tested functionality. The remaining extra delay is an accepted release non-blocker.

Optimize it after release with explicit stage timing rather than removing established 44.1 startup behavior blindly.

## Rationale / release policy

```text
requested rate already active
 -> do not rewrite the same rate before ISO startup

real rate transition required
 -> set both AV/C directions and verify by STATUS

44.1 ISO startup
 -> keep rate-specific lead
 -> service audio during the lead
 -> perform proven post-start 44.1 reassert
 -> expose meter listener only after reassert

48 ISO startup
 -> shorter lead
 -> no post-start reassert

local IPC client disappears
 -> never terminate transport with SIGPIPE

clean 44.1 stop
 -> leave hardware at 44.1

44.1 engine failure
 -> retain best-effort 48 kHz recovery restore

normal validation
 -> use launchd-managed service path
```

This policy preserves the proven low-latency scheduler at both rates while keeping the FW410-specific 44.1 startup ritual isolated from ordinary 48 kHz operation.
