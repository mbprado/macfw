# FW1814 native 44.1 kHz transport success

Date: 2026-09-08

This records hardware validation of the M-Audio FireWire 1814 native 44.1 kHz blocking AMDTP path on macOS.

## Proven rate control

The existing guarded FW1814 initializer was executed at 44100 Hz after a clean product-scoped FireWire bus reset. Hardware result:

- documented M-Audio clock/digital baseline: PASS;
- OUTPUT PLUG SIGNAL FORMAT 44100: PASS;
- required 100 ms OUTPUT -> INPUT delay: PASS;
- INPUT PLUG SIGNAL FORMAT 44100: PASS;
- authoritative INPUT STATUS readback: 44100 Hz.

## Native blocking transport

The 44.1 kHz path follows the Linux/ALSA blocking AMDTP timing model rather than reusing the simple 48 kHz 8/8/8/NODATA pattern.

Validated S/PDIF-mode stream geometry:

- playback host -> device: 6 PCM + 1 MIDI, DBS=7;
- capture device -> host: 10 PCM + 1 MIDI, DBS=11;
- FDF=0x01 at 44.1 kHz;
- eight AM824 events in each data-bearing packet;
- 441 data-bearing packets per 640 FireWire cycles;
- TX ring 640 packets / 320-packet halves;
- RX ring 256 packets;
- scheduled TX lead 2048 cycles (256 ms).

The first full-duplex hardware probe completed the FW1814-specific post-start sequence while continuously servicing the dynamic 44.1 TX state:

1. start host->device playback ISO;
2. start device->host capture ISO;
3. service through the scheduled first TX cycle;
4. OUTPUT 44100 CONTROL;
5. continue servicing TX for the required 100 ms delay;
6. INPUT 44100 CONTROL;
7. authoritative INPUT readback 44100.

A matching AV/C INTERIM response was observed and correctly treated as non-terminal before the final ACCEPTED response.

## Capture cadence proof

After the initial NODATA-only settling snapshot, repeated 256-slot snapshots showed the expected native 44.1 data/NODATA ratio with no malformed packets:

```text
snapshot 2: 177 data, 79 NODATA, 0 invalid
snapshot 3: 176 data, 80 NODATA, 0 invalid
snapshot 4: 176 data, 80 NODATA, 0 invalid
snapshot 5: 176 data, 80 NODATA, 0 invalid
snapshot 6: 176 data, 80 NODATA, 0 invalid
snapshot 7: 176 data, 80 NODATA, 0 invalid
snapshot 8: 176 data, 80 NODATA, 0 invalid
```

Observed data packets were 360 bytes with DBS=11, FMT=0x10 and FDF=0x01. Eight-byte NODATA packets used SYT=0xffff. DBC advanced by eight only on data-bearing packets and remained continuous across NODATA cycles, including wrap through 248 -> 0.

The very first snapshot contained 256 valid NODATA packets while the FW1814 settled after the post-start rate kick. This is a startup state, not malformed transport data.

## Analog playback proof

A dedicated preloaded native-44.1 tone probe used the already-confirmed FW1814 analog routing and physical output map while keeping the 640/320 dynamic TX refill active.

All four physical analog outputs were hardware-tested with 440 Hz / -24 dBFS and played cleanly. The existing map remains valid at 44.1 kHz:

```text
Analog Output 1 <- raw PCM position 2
Analog Output 2 <- raw PCM position 3
Analog Output 3 <- raw PCM position 0
Analog Output 4 <- raw PCM position 1
```

A boundary-unfriendly 523.25 Hz tone was also hardware-confirmed clean. Representative successful output:

```text
TX halves refilled:       82
TX data packets refilled: 18081
TX tone frames:           144648
TX silence frames:        0
PCM remaining frames:     72324
status: PASS
```

Therefore the short-live-ring distortion previously found at 48 kHz does not recur in the native 44.1 path with the validated 640/320 geometry.

## Hardware-validated milestone

Native FW1814 44.1 kHz transport is now proven for:

- guarded 44.1 sample-rate initialization;
- native blocking AMDTP timing;
- full-duplex CMP/ISO startup;
- correlated OUTPUT/INPUT FCP controls;
- stable capture data/NODATA cadence;
- continuous DBC behavior;
- all four analog playback outputs;
- clean 440 Hz playback;
- clean 523.25 Hz playback.

The next step is manual CoreAudio integration at 44.1 kHz. Automatic supervisor rate switching should remain deferred until CoreAudio playback and capture are independently validated at 44.1 kHz.
