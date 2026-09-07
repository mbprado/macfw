# FW1814 integrated 48 kHz analog engine success

Date: 2026-09-07

This records the first successful production-style dynamic full-duplex analog path for the M-Audio FireWire 1814 on macOS.

## Scope

The integrated engine runs the already-proven 48 kHz / internal-clock / S/PDIF-mode transport and exposes only hardware-confirmed analog I/O:

- CoreAudio-facing playback: Analog Outputs 1-4;
- CoreAudio-facing capture: Analog Inputs 1-8;
- S/PDIF stream positions remain hidden pending later cross-device verification;
- MIDI remains deferred;
- explicit headphone routing remains deferred.

## Startup

The integrated `fw1814analog48` engine successfully completed:

- documented M-Audio special mixer routing;
- full-duplex CMP connection;
- playback ISO start before capture ISO start;
- 50 ms ISO settle;
- correlated OUTPUT PLUG SIGNAL FORMAT CONTROL at 48000 Hz;
- 100 ms delay;
- correlated INPUT PLUG SIGNAL FORMAT CONTROL at 48000 Hz.

Observed startup result:

```text
FW1814 stream kick: PASS (matched INTERIMs=0, ignored unrelated=0)
FW1814 analog engine ONLINE
```

The correlated FCP layer is now used instead of accepting the first FCP response from the node. Matching INTERIM responses remain non-terminal and unrelated responses are ignored.

## Dynamic capture success

The receive ring uses the hardware-proven 64-slot geometry. With the corrected 11-quadlet event width (10 PCM + 1 MIDI), the live capture path ran at exactly 48 kHz.

Representative 2-second statistics while streaming:

```text
capture delta: 96000 frames
rx-touched:    64/64
malformed:     0
invalid:       0
dbc-gap:       0
reorder:       0
stale:         0
```

Before a consumer attaches, the capture ring fills to capacity as expected. Once the test consumer begins issuing read calls, the producer activates live capture and the queue remains near empty while the consumer drains it.

### Physical Input 1 verification

With a steady signal injected into physical Analog Input 1, the CoreAudio-facing capture meter reported approximately:

```text
A1 ~= 0.1346
A2 ~= 0.00001
A3..A8 ~= 0.00003
```

This independently confirms the production decoder/remap path preserves the previously measured physical Input 1 mapping.

## Dynamic playback success

All four physical analog outputs were already confirmed through the dynamic SHM -> PCM -> AMDTP path.

The physical order remains:

```text
Analog Output 1 <- raw PCM position 2
Analog Output 2 <- raw PCM position 3
Analog Output 3 <- raw PCM position 0
Analog Output 4 <- raw PCM position 1
```

A temporary distortion observed during early dynamic playback testing was traced to the SHM test producer, not the FireWire scheduler: it wrote 240-frame bursts while the transport refilled 384-frame halves, causing zero-fill inside active tone periods.

After changing the test producer to buffer ahead, a 3-second tone delivered exactly 144000 audio frames and sounded clean. `tx-late` remained stable, confirming the distortion was not caused by missed DCL refill deadlines.

Representative result:

```text
tx-audio: 144000 frames after one 3-second tone
tx-late:  2 and stable
```

Idle `tx-silence` continues to increase outside active playback by design.

## Proven milestone

The FW1814 now has a hardware-proven dynamic 48 kHz full-duplex analog transport suitable for connection to a CoreAudio HAL layer:

- 4 verified analog playback channels;
- 8 verified analog capture channels;
- correct blocking AMDTP cadence;
- continuous dynamic PCM playback;
- continuous 48 kHz capture;
- generation-safe teardown;
- correlated FCP rate controls.

The next integration step is a 48 kHz-only AudioServerPlugIn exposing these 4 output and 8 input channels while keeping the FireWire transport process manual for the first CoreAudio validation.
