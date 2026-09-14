# FW410 isochronous transport bring-up

This document records the hardware-confirmed IEEE 1394/CIP/AMDTP transport behavior used by the current macfw FW410 runtime.

## Confirmed transport path

The following path is experimentally confirmed in user space on Intel macOS:

1. detect `FW Bootloader`;
2. execute the guarded boot-from-flash cue;
3. wait for re-enumeration as operational `FW 410`;
4. perform raw FCP/AV/C command/response transport;
5. discover stream topology and supported formations;
6. allocate FireWire IRM channel/bandwidth resources;
7. establish both CMP directions and restore the exact PCR state on exit;
8. run local NuDCL receive/transmit programs;
9. decode AM824 MBLA capture to signed 24-bit PCM;
10. transmit native 48 kHz and 44.1 kHz playback PCM;
11. feed/consume CoreAudio PCM through shared memory.

The FW410 requires packet flow in both directions for normal sample-bearing capture. Two CMP connections alone are not enough.

## 48 kHz device-to-host capture

Observed data-bearing formation:

```text
FMT  0x10 (AM824)
FDF  0x02 (48 kHz)
DBS  5
packet length 168 bytes
8 events per data-bearing packet
```

Raw positions:

```text
1  S/PDIF In L
2  Analog In 1
3  S/PDIF In R
4  Analog In 2
5  MIDI
```

The first four positions carry AM824 MBLA words; the low 24 bits are signed PCM. The fifth position is the MIDI slot.

At 48 kHz the FW410 uses the expected blocking cadence: three 8-event data packets for every NODATA packet. This yields exactly 48,000 sample events/sec:

```text
3 * 8 events / (4 * 125 us) = 48,000 events/sec
```

DBC advances by eight on each data-bearing packet and is retained across NODATA.

## Physical input validation

A controlled signal on Analog Input 1 was previously confirmed at raw PCM position 2 while Analog Input 2 remained near the noise floor and both S/PDIF positions remained silent. This established the physical-to-stream mapping used by the CoreAudio capture layer.

## Host-to-device playback

At 44.1/48 kHz playback is 10 PCM positions plus one MIDI slot (`DBS=11`). The transport permutes CoreAudio's physical/user-facing output order into the FW410 raw stream order documented in `stream-topology.md`.

Native 48 kHz playback is clean with the current 640-cycle TX ring / 320-cycle refill geometry. Native 44.1 playback is also clean after the FW410-specific post-start AV/C 44.1 reassertion.

## Capture requires duplex AMDTP

The FW410 does not continue real capture when only the device-to-host side is active. A valid host-to-device AMDTP stream must be continuously serviced.

For isolated capture testing, `capturebridge48000` therefore runs the real 48 kHz transmit scheduler with an empty 10-channel PCM FIFO. It sends correctly timed digital silence while the receive side delivers actual capture samples.

The production full-duplex runtime should replace this silence with the normal CoreAudio playback ring rather than remove the transmit scheduler.

## NuDCL receive publication evolution

### Full-ring publication — corrupted

The early receive abstraction published receive metadata for all 256 DCL slots only at the end of a ring revolution. At 8,000 FireWire cycles/sec, this exposed receive data in ~32 ms batches.

Although packets decoded and capture levels looked correct, recorded audio was badly broken. The failure was temporal: by the time userspace scanned the full batch, early payload slots could already be reused by the next DMA revolution.

### 32-cycle publication — almost clean

Changing `AmdtpReceiveRing` to publish metadata every 32 slots reduced the visibility interval to ~4 ms and made capture almost clean.

A global userspace scan still gathered all changed slots from the 256-slot ring into one candidate set and reordered them by AMDTP DBC. This fixed most corruption but still allowed independently published groups to be combined around a ring/update boundary.

Controlled test 19 remained subjectively very good but still contained occasional small cracks.

### Terminal-slot completed groups — validated clean

The final rule does not globally infer freshness.

For each 32-slot receive group:

1. the group's terminal receive DCL executes `SetDCLUpdateList` for that group's metadata;
2. userspace watches the terminal slot's `(timestamp, isoHeader)` signature;
3. a changed terminal signature means that exact group's publication completed;
4. userspace snapshots only those 32 slots;
5. DBC continuity is validated/ordered only inside that group.

No mixed send/receive completion-marker DCL is used. The receive program remains receive-only.

Representative final status:

```text
capture frames=76064 (delta 76064)
active=1 queued=4096 drops=0
malformed=0 invalid=0
chunks=481 dbc-gap=0 ts-back=0 reorder=0 stale=0

capture frames=172064 (delta 96000)
active=1 queued=4096 drops=0
malformed=0 invalid=0
chunks=981 dbc-gap=0 ts-back=0 reorder=0 stale=0
```

The steady-state producer cadence then remained at exactly 96,000 frames per two-second report. `chunks` increased by ~500 per report, matching:

```text
8000 cycles/sec / 32 cycles/group = 250 groups/sec
```

The final controlled recording had no significant steady-state discontinuities.

## Shared capture ring and prefill

Decoded capture is written into a separate four-channel Float32 POSIX shared-memory ring consumed by the HAL `ReadInput` callback.

The producer waits for HAL consumer activity and accumulates 4,096 frames (~85 ms) before setting the capture ring active. This controlled prefill prevents startup zero-fill from being mistaken for a steady-state transport problem.

During the validated final run:

- shared-ring `drops` stayed zero;
- capture queue remained around 4k frames;
- `dbc-gap`, `reorder`, and `stale` stayed zero;
- small timestamp-regression diagnostics did not correspond to audible/sample-level discontinuities.

## Cleanup and lifecycle

Transport experiments save the exact original oPCR0/iPCR0 values and restore them during cleanup. This behavior must remain in the production stream engine.

Rate changes, device boot, reconnects and bus resets can change FireWire generation/node identity. Long-running code must reacquire the device rather than retain stale node/generation assumptions.

## Current next work

The basic 48 kHz transport is no longer a proof-of-concept blocker. The next transport milestone is to combine the proven receive engine with the proven real playback engine in one rate-aware full-duplex process:

```text
10-channel CoreAudio playback SHM
              ->
        full-duplex FW410 transport
              ->
4-channel CoreAudio capture SHM
```

After native 48 kHz full duplex is stable, integrate native 44.1 capture while preserving the already-confirmed M-Audio 44.1 startup quirk.
