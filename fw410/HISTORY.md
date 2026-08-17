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
