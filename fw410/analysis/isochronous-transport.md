# FW410 isochronous transport bring-up

Last updated: 2026-08-19

This document summarizes the hardware-confirmed user-space IEEE 1394 isochronous transport behavior of the M-Audio FireWire 410 on Intel macOS.

## Confirmed path

The following sequence works entirely from user space:

1. detect the `FW Bootloader` personality;
2. execute the guarded boot-from-flash cue with `fwboot --execute`;
3. wait for re-enumeration as operational `FW 410`;
4. perform FCP/AV/C control;
5. allocate IRM channel/bandwidth resources;
6. establish both CMP directions;
7. run cyclic NuDCL receive and transmit programs;
8. decode device-to-host AM824 capture;
9. encode and schedule host-to-device AM824 playback;
10. restore exact original PCR state on exit.

The FW410 requires real packet flow in both directions. Established CMP connections alone are insufficient for sample-bearing capture.

## 48 kHz capture formation

Data-bearing capture packets are 168 bytes:

```text
8-byte CIP header
+ 8 events * 5 positions * 4 bytes
= 168 bytes
```

Observed formation:

- FMT `0x10` — AM824
- FDF `0x02` — 48 kHz
- DBS `5`
- 8 sample events per data packet
- repeating 3 data / 1 NODATA blocking cadence

Raw stream positions:

```text
1  S/PDIF In L
2  Analog In 1
3  S/PDIF In R
4  Analog In 2
5  MIDI
```

The four audio positions use MBLA label `0x40`; the low 24 bits are signed PCM. The MIDI position carries AM824 MIDI/no-data words.

The 3:1 cadence produces exactly 48 kHz:

```text
3 * 8 events / (4 * 125 us) = 48,000 events/sec
```

DBC advances by eight for each data-bearing packet and is retained across NODATA packets.

## 48 kHz playback formation

At 44.1/48 kHz, host playback uses ten PCM positions plus one MIDI position (`DBS=11`). The maximum blocking-mode packet is 360 bytes:

```text
8-byte CIP header
+ 8 events * 11 positions * 4 bytes
= 360 bytes
```

Valid host-to-device AMDTP, including correctly timed digital silence, is enough to keep FW410 capture sample-bearing.

Clear 48 kHz playback required a 640-cycle TX ring with two 320-cycle refill halves. The earlier 128/64 geometry did not provide enough service margin under the HAL workload.

## Native 44.1 kHz

Native 44.1 playback is accepted only after the FW410-specific startup sequence:

1. select 44.1 kHz in both AV/C directions;
2. establish duplex CMP and ISO;
3. begin valid native 44.1 AMDTP;
4. reassert 44.1 kHz on both directions while streaming is live;
5. continue the normal blocking schedule.

The startup reassertion is device-specific and must not be hidden in the generic packet generator.

## Receive publication and capture quality

### Full-ring publication failure mode

The first reusable receive ring published metadata for all 256 slots only at the final DCL. At 8,000 FireWire cycles per second this exposed roughly 32 ms batches.

Capture decoded correctly and reached CoreAudio, but sounded broken. The shared PCM ring was healthy and showed no drops; corruption occurred because userspace could read slot payloads while early DMA locations were already being reused in the next cyclic revolution.

### 32-cycle publication

Publishing metadata in 32-slot groups reduced the exposure interval to approximately 4 ms and made the recording almost clean.

However, scanning every changed slot across all 256 descriptors could still combine groups published at different times. Global DBC ordering repaired much of this but left occasional small discontinuities.

### Completed-group consumption

The validated solution preserves a receive-only DCL program:

- each 32-slot group has its update list attached to the group's terminal receive DCL;
- userspace watches the terminal slot's `(timestamp, isoHeader)` signature;
- when that signature changes, the exact 32-slot group is considered completed;
- only that group is snapshotted;
- DBC continuity and ordering are applied inside that completed group.

This prevents mixed-generation payload snapshots.

Representative final status:

```text
capture frames=940064 (delta 96000)
queued=3968
drops=0 malformed=0 invalid=0
chunks=4981
dbc-gap=0 ts-back=4 reorder=0 stale=0
```

A controlled 1 kHz source produced no significant steady-state discontinuities after this change.

See [`capture-pipeline.md`](capture-pipeline.md) for the full CoreAudio capture architecture and quality comparison.

## Capture prefill

The capture producer initializes the shared ring inactive, waits for HAL `ReadInput`, accumulates 4,096 frames and then activates capture with an approximately 85 ms live-edge cushion.

This handles startup sequencing without masking receive-order defects or growing latency indefinitely.

## Cleanup behavior

Transport tools save original oPCR0/iPCR0 values and restore them on normal exit and guarded failure paths. This requirement must remain in the persistent runtime.

## Remaining transport work

1. merge the proven capture path with real ten-channel playback in the normal rate-aware transport;
2. validate sustained full-duplex operation;
3. add native 44.1 capture;
4. add bus-reset, generation-change and reconnect recovery;
5. move boot/rate/CMP/ISO lifecycle into an automatically started companion service;
6. preserve completed-group receive publication in future abstraction work.
