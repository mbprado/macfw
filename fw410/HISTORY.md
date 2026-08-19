# FW410 project history

This file records visible project milestones rather than every diagnostic experiment. Detailed protocol and transport findings remain under `fw410/analysis/`.

## 2026-08-17 — First native macOS audio device

The macfw HAL AudioServerPlugIn was accepted by CoreAudio and appeared as **M-Audio FireWire 410** in both Audio MIDI Setup and the normal macOS audio output-device selector.

At this milestone:

- the device can be selected as the default and system output;
- CoreAudio starts and stops the device I/O thread successfully;
- stereo output is published;
- 44.1 kHz and 48 kHz are selectable;
- the HAL device is still synthetic at this exact checkpoint, so `WriteMix` accepts but discards PCM and no physical FW410 audio is produced yet.

Screenshot: [`pictures/screenshot1.png`](pictures/screenshot1.jpg)

![First macfw FW410 CoreAudio device milestone](pictures/screenshot1.jpg)

This milestone proves the user-space HAL approach can expose the legacy FireWire 410 as a normal selectable macOS audio interface without requiring DriverKit provisioning. The next milestone is routing the HAL `WriteMix` PCM stream through shared memory into the already-proven native 44.1 kHz FireWire/AMDTP transport.

## 2026-08-17 — First clean hardware-backed CoreAudio playback

The complete 44.1 kHz playback path worked end to end with normal macOS audio applications and produced clear audio on physical FW410 Analog Outputs 1 and 2.

The proven path at this checkpoint is:

`macOS application -> CoreAudio HAL -> WriteMix -> shared-memory stereo Float32 ring -> halbridge44100 -> 10-channel FW410 PCM mapping -> PcmRingBuffer -> AmdtpPcmStream44100 -> FireWire ISO -> M-Audio FireWire 410`

Observed results:

- CoreAudio continuously supplied 192-frame `WriteMix` buffers;
- the HAL shared ring was consumed continuously by `halbridge44100`;
- live frame deltas were approximately 88,128-88,896 frames per two-second reporting interval, consistent with the 44.1 kHz stream;
- the FW410-specific post-start AV/C 44.1 kHz reassertion succeeded on both OUTPUT and INPUT plug 0;
- audio on Analog Outputs 1 and 2 was reported clear;
- Ctrl-C followed the guarded ISO/CMP cleanup path and the device read back successfully at 48 kHz after restoration.

This is the first milestone where the macfw CoreAudio device is not merely visible to macOS: ordinary application audio reaches the real FireWire hardware cleanly.

## 2026-08-18 — Native 48 kHz CoreAudio playback

The same HAL/shared-memory architecture was validated at native 48 kHz with no sample-rate conversion.

The initial 48 kHz implementation sounded broken even though the HAL producer cadence was correct. A controlled A/B test isolated the decisive difference to FireWire transmit scheduling margin: the old 128-cycle TX ring with 64-cycle refill halves provided only about 8 ms per half under this workload, while enlarging the ring to 640 cycles with 320-cycle halves provided about 40 ms. With the larger geometry, native 48 kHz audio became clear.

The committed path also uses a 16,384-frame PCM FIFO, drains shared-memory backlog until caught up, requests user-interactive pthread QoS, and avoids redundant AV/C rate CONTROL when the FW410 is already at 48 kHz.

Small load-sensitive dropouts can still occur during unrelated desktop activity. Current diagnostics show zero scheduler-inserted silence at those moments and no distinctive spike in `lateCyclePolls`, so this remains a transport-service jitter issue rather than a proven AMDTP underrun.

With both native 44.1 and 48 kHz playback proven, development moved to a rate-aware `haltransport` supervisor that selects the matching native engine from the HAL-selected CoreAudio rate without rewriting either known-good packet path first.

## 2026-08-18 — Full 10-channel CoreAudio playback validated in Logic Pro

The HAL output stream was expanded from the temporary stereo presentation to the FW410's full 10-channel host playback topology using the mapping already documented in `analysis/stream-topology.md`.

CoreAudio now presents the channels in physical/user-facing order:

1. Analog Out 1
2. Analog Out 2
3. Analog Out 3
4. Analog Out 4
5. Analog Out 5
6. Analog Out 6
7. Analog Out 7
8. Analog Out 8
9. S/PDIF Out L
10. S/PDIF Out R

The transport explicitly permutes these into the FW410's BridgeCo/AMDTP slot order rather than exposing the unusual raw stream positions to applications.

Hardware validation was performed in Logic Pro X. Independent output routing confirmed that **all eight analog outputs and both S/PDIF output channels play correctly**. Ordinary stereo playback continues to land on Analog Outputs 1/2 as intended.

Logic Pro also produced noticeably fewer audible dropouts than browser/YouTube playback during the same development state. This suggests the remaining occasional glitches are influenced by client/system scheduling or buffering as well as the transport service, and are not a blocker for the playback architecture.

This milestone establishes the full FW410 playback side as functionally usable from a multichannel CoreAudio application.

## 2026-08-19 — First end-to-end CoreAudio capture in Logic Pro

The first real FW410 input signal was successfully recorded through the complete user-space capture path at 48 kHz:

Screenshot: [`pictures/screenshot2.png`](pictures/screenshot2.jpg)

![First macfw FW410 CoreAudio capture milestone](pictures/screenshotr2.jpg)  

`FW410 Analog In 1 -> FireWire ISO receive -> AMDTP/MBLA decode -> 4-channel shared capture ring -> AudioServerPlugIn ReadInput -> Logic Pro`

At this milestone:

- CoreAudio exposes four input channels: Analog In 1/2 and S/PDIF In L/R;
- the standalone 48 kHz capture transport continuously decodes approximately 95k-97k frames per two-second interval after startup;
- the shared capture ring is now genuinely drained by CoreAudio rather than filling to its 32,768-frame capacity;
- `droppedFrames` remained zero during the successful recording run;
- one capture diagnostic snapshot showed 866,512 frames delivered from the shared ring to CoreAudio out of 886,912 requested frames;
- 20,400 frames were zero-filled, approximately 2.3% of requested input and roughly 425 ms at 48 kHz;
- the recorded signal was recognizable but audibly broken, similar to the early playback transport before its buffering/scheduling margin was increased.

The first producer interval was also short by approximately the same amount as the total zero-fill deficit, indicating a strong startup/prefill component rather than a persistent 2.3% steady-state loss. Subsequent producer intervals were essentially at the expected 48 kHz cadence, while the shared queue oscillated in a healthy low-thousands range.

Development therefore moved to a capture pre-roll experiment: keep the capture ring inactive until CoreAudio is actually issuing `ReadInput`, accumulate a controlled 4,096-frame (~85 ms) cushion, discard older startup backlog beyond that cushion, and only then enable live capture consumption. The goal is to separate startup starvation from any remaining steady-state NuDCL scheduling jitter without changing the now-proven HAL topology or shared-memory lifecycle.
