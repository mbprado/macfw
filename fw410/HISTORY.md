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

Screenshot: [`pictures/screenshot1.png`](pictures/screenshot1.png)

![First macfw FW410 CoreAudio device milestone](pictures/screenshot1.png)

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
