# FW410 CoreAudio capture pipeline

Last updated: 2026-09-05

This document records the current hardware-validated native capture path used by the production full-duplex 44.1/48 kHz transport.

## Validated result

Native capture is working end to end at **44.1 kHz and 48 kHz** through the macfw AudioServerPlugIn and launchd-managed full-duplex transport.

```text
FW410 Analog/S/PDIF input
    -> device-to-host FireWire ISO
    -> NuDCL receive ring
    -> completed 32-cycle publication groups
    -> AMDTP/CIP validation and DBC continuity
    -> AM824 MBLA-24 decode
    -> CoreAudio channel permutation
    -> four-channel Float32 shared capture ring
    -> AudioServerPlugIn ReadInput
    -> CoreAudio application / Logic Pro
```

CoreAudio-facing input order:

1. Analog In 1
2. Analog In 2
3. S/PDIF In L
4. S/PDIF In R

Raw FW410 AMDTP audio order:

1. S/PDIF In L
2. Analog In 1
3. S/PDIF In R
4. Analog In 2
5. MIDI slot alongside the audio payload

The transport explicitly permutes the four audio positions into CoreAudio order.

## Shared-memory capture ABI

The capture path uses persistent POSIX shared memory:

```text
/macfw_fw410_capture_v2
```

The current structure ABI is versioned independently inside `hal/include/macfw_hal_capture_shm.h` and carries:

- four interleaved Float32 channels;
- 32,768-frame capacity;
- monotonic write/read frame counters;
- producer state and sample rate;
- packet/decode diagnostics;
- queue extrema/underrun diagnostics;
- HAL `ReadInput` consumption and zero-fill diagnostics.

The HAL establishes a stable mapping outside the real-time callback. Each native engine reuses and reinitializes the same persistent object in place on startup, preventing stale producer/consumer mapping splits.

## Full-duplex requirement

The FW410 capture path depends on valid host-to-device AMDTP continuing at the same time. Production capture is therefore not a standalone receive bridge: both directions are established and serviced in one native full-duplex engine.

At both rates the audio service thread owns the steady-state capture/playback work:

```text
capture publication/decode
 -> capture SHM publication
 -> playback SHM -> PCM pumping
 -> TX refill/service
 -> meter accumulation
```

The isoch callback dispatcher runs on its own dedicated run-loop thread.

## Capture prefill — current baseline

The original capture implementation used a 4,096-frame queue, which produced roughly 85-93 ms of steady capture latency. That value is obsolete.

Hardware testing progressively reduced the prefill and established **256 frames** as the current validated stability/latency baseline.

Approximate prefill duration:

```text
44.1 kHz: 256 frames ~= 5.8 ms
48 kHz:   256 frames ~= 5.3 ms
```

Activation sequence:

1. initialize the capture ring inactive;
2. wait until HAL `ReadInput` activity is observed;
3. accumulate at least the configured 256-frame prefill;
4. position the read cursor at the controlled live-edge cushion;
5. publish capture active.

This prefill is only one internal latency component. It must not be equated with total input, output or round-trip latency.

## Prefill experiments

Historical hardware tests established the current choice:

- 4096 frames — very stable but ~90 ms queue, unacceptable for software monitoring;
- 2048 — stable, lower but still high latency;
- 1024 — stable and improved;
- 512 — bad queue behavior / HAL zero fill;
- 256 — best validated low-latency baseline;
- lower experimental values did not improve the effective result reliably and in some cases worsened queue behavior.

Do not lower or raise the 256-frame baseline casually before release. In particular, do not increase it merely to hide scheduler stalls.

## NuDCL receive publication rule

The working receive rule remains:

- capture receive ring: 256 FireWire cycles;
- each publication group: 32 receive DCL slots;
- the terminal receive DCL publishes that group's metadata update list via `SetDCLUpdateList`;
- userspace treats a changed terminal-slot `(timestamp, isoHeader)` signature as the completed-group token;
- only the exact 32 slots in that group are snapshotted/decoded;
- AMDTP DBC continuity and ordering are applied inside the completed group.

The expected publication cadence is:

```text
8000 FireWire cycles/sec / 32 cycles/group = 250 groups/sec
```

A 16-cycle grouping experiment at 44.1 kHz produced deterministic frame loss/DBC gaps and was reverted. **32 cycles is the validated production grouping.**

## Real-time scheduling

The main remaining capture discontinuities were not solved by adding queue depth; they were strongly tied to userspace service starvation.

The release baseline therefore uses:

- dedicated isoch callback thread with USER_INTERACTIVE QoS;
- dedicated audio service thread;
- 250 us `mach_wait_until` pacing;
- USER_INTERACTIVE QoS;
- `THREAD_TIME_CONSTRAINT_POLICY`:
  - period 2000 us;
  - computation 500 us;
  - constraint 2000 us;
  - preemptible true.

This scheduling architecture is hardware-validated at both 44.1 and 48 kHz.

44.1 kHz also services capture/playback/TX during its startup lead and post-start reassert interval so the rings are not left unserviced before READY.

## Hardware validation method

The main low-latency/stability regression path is a physical round trip:

```text
YouTube / CoreAudio playback
 -> FW410 outputs 1/2
 -> physical cable
 -> FW410 inputs 1/2
 -> CoreAudio capture
 -> Logic software monitoring
 -> CoreAudio playback
 -> FW410 outputs 3/4
```

For relative latency comparison, FW410 hardware direct monitoring can be enabled simultaneously with Logic monitoring. Audible flamming/echo between the direct and software-monitored paths provides a useful subjective round-trip comparison.

With the current scheduler:

- 44.1 kHz latency is excellent and cutoffs are reduced to a practically solved level in normal service operation;
- 48 kHz is likewise very stable and has slightly lower perceived round-trip latency.

## Diagnostics

The production runtime/status path can report:

- capture frames decoded/published;
- active state;
- queued frames;
- queue extrema;
- shared-ring drops;
- malformed packets;
- invalid MBLA labels;
- completed 32-cycle groups (`chunks`);
- DBC gaps;
- timestamp-back observations (`ts-back`);
- reorder/stale counts;
- HAL read calls/requested/consumed frames;
- underrun events;
- HAL zero-filled frames;
- playback/TX late and silence counters where verbose diagnostics are enabled.

Small `ts-back` increases have not correlated with audible failures or frame deficits in the known-good scheduler and are treated as a secondary timestamp diagnostic rather than a fault by themselves.

## HAL underrun caveat

The HAL currently advances the capture read cursor only by frames actually obtained from the shared ring, while CoreAudio's timeline advances for the entire requested buffer and missing frames are zero-filled.

After a real underrun, this can create timeline/backlog debt rather than an immediate clean resynchronization. The issue is known, but the current scheduler has made real underruns rare enough that it is **not** part of the release stabilization work.

A future fix should implement a safe resync/rebuffer policy; do not simply advance the read cursor beyond `writeFrame`.

## Foreground verbose testing caveat

`MACFW_VERBOSE=1` is useful diagnostically but is not performance-neutral: large periodic verbose reporting is executed from the real-time audio service path. Normal launchd-managed service operation is therefore the authoritative release test path.

## Solved vs deferred

Solved / release baseline:

- 44.1 and 48 kHz FireWire capture;
- MBLA decode and physical channel mapping;
- persistent capture SHM lifecycle;
- CoreAudio `ReadInput` delivery;
- 256-frame low-latency prefill;
- 32-cycle completed-group receive consistency;
- full-duplex playback/capture integration;
- launchd startup and runtime recovery;
- dedicated real-time scheduling at both rates;
- live capture meter feed to the Inputs tab.

Deferred:

- calibrated CoreAudio latency/safety-offset reporting;
- robust HAL resync policy after a genuine capture underrun;
- further latency optimization only if it can preserve the current stability baseline.

The release rule is to preserve the current 256-frame prefill, 32-cycle grouping and proven scheduler unless a reproducible regression provides evidence for a change.
