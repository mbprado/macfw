# FW1814 dynamic 44.1/48 kHz CoreAudio success

Date: 2026-09-08

This records hardware validation of the complete M-Audio FireWire 1814 analog
playback path on macOS with CoreAudio-controlled 44.1/48 kHz switching,
supervised transport selection, and reconnect recovery.

## Validated checkpoints

The completed path is split into three narrow checkpoints:

- `b08fa8ad858f81438e5002423b679c42c9f42599` restores the proven native
  44.1 kHz startup sequence and makes its 88200-frame silent preload the
  default;
- `145ff47c8892bd66593353e165c93fc9d8049654` makes the supervisor select and
  restart the 44.1 or 48 kHz transport engine from the SHM rate request;
- `875a6638b9394ca66777458ebb36ab9542b70fc4` exposes both nominal rates to
  CoreAudio and publishes configuration changes to the supervisor.

Neither rate switch integration step changes the hardware-proven AMDTP packet
formation inside `fw1814analog44` or `fw1814analog48`.

## Required 44.1 kHz startup behavior

Hardware A/B testing established that 44.1 kHz playback requires 88200 frames
of PCM-backed digital silence to be preloaded before ISO setup. The successful
sequence then enters the ordinary audio service loop immediately after the
post-start INPUT rate readback. The preload drains naturally through that loop.

Attempts to replace this with a separate warm-up state machine, a small-ring
refill loop, a post-warm-up PCM reset, or a barrier before declaring the engine
online all produced distorted playback even when transmitted frame counters
looked exact. The proven preload-and-normal-loop ordering must therefore remain
unchanged unless new hardware tests demonstrate an equivalent sequence.

At startup, the first service statistic reports 84672 PCM frames rather than
88200 because the initial 640-packet TX-ring prime already consumed 3528
frames. This is expected:

```text
88200 - 3528 = 84672
```

## Supervisor rate handoff

The supervisor accepts only 44100 or 48000 from the versioned playback SHM
object. When the request changes it:

1. sends SIGTERM to the active transport and waits for clean teardown;
2. requires the existing guarded, product-scoped bus reset before restart;
3. runs `fw1814init` at the newly requested rate;
4. launches `fw1814analog44` or `fw1814analog48` as appropriate;
5. continues using the existing bootloader and reconnect recovery path.

Both switch directions were hardware-tested successfully with clean audio.

## HAL handoff behavior

The AudioServerPlugIn reports two discrete nominal rates and matching physical
and virtual stream formats for both input and output. A CoreAudio configuration
change discards stale playback/capture backlog, marks the current transport
inactive, and publishes the new playback rate as the supervisor trigger.

The transport engine owns the playback `active` flag. During a handoff the HAL
drops output until the new engine marks itself online. Capture zero-fills until
the new engine reinitializes the capture ring at the requested rate. This keeps
new-rate PCM out of the old-rate transport during the restart window.

## Hardware validation

The integrated result was tested from Audio MIDI Setup and normal CoreAudio
applications:

```text
Audio MIDI Setup 48000 -> 44100: PASS
Audio MIDI Setup 44100 -> 48000: PASS
repeated tone playback:          clean
YouTube/CoreAudio playback:      clean
disconnect/reconnect recovery:   PASS
selected rate after reconnect:   restored
audio after reconnect:           clean
```

Earlier decisive 44.1 kHz SHM tests also confirmed clean 440 Hz and 523.25 Hz
tones. The arbitrary-frequency tests remain mandatory regressions because they
expose packet-ring boundary errors that phase-aligned tones can hide.

## Current boundary

The hardware-proven scope is now:

- analog outputs 1-4;
- analog inputs 1-8 in the full-duplex transport;
- 44.1 and 48 kHz native blocking AMDTP engines;
- CoreAudio nominal-rate selection in both directions;
- automatic engine selection, guarded restart, and reconnect recovery;
- clean tone and real-program playback at both rates.

S/PDIF exposure, exact digital L/R presentation, explicit headphone routing,
MIDI, and rates above 48 kHz remain deferred. Dedicated long-run CoreAudio
capture validation at 44.1 kHz should be recorded separately from this playback
and lifecycle milestone.
