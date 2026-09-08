# M-Audio FireWire 1814

The FW1814 is the second macfw device target.

## Current scope

The first bring-up intentionally mirrors the proven FW410 release scope:

- Intel macOS;
- 44.1 kHz and 48 kHz only;
- PCM audio first;
- capture before playback;
- full duplex after the individual directions are validated;
- CoreAudio/HAL integration after raw transport is stable;
- MIDI deferred;
- 88.2/96/176.4/192 kHz deferred;
- mixer/control writes deferred until audio is reliable.

## Architecture

FW1814 development is also the beginning of macfw's explicit multi-device layout:

- reusable transport/CoreAudio primitives migrate to `common/` only as they are required by both devices;
- FW1814-specific stream geometry, clock/digital-mode handling, control protocol and GUI live under `devices/fw1814/`;
- the released FW410 implementation remains the regression reference while this extraction happens.

The experimental FW1814 profile now has hardware-validated analog full-duplex
transport and CoreAudio integration at both 44.1 and 48 kHz. Audio MIDI Setup
can switch the nominal rate in either direction, the supervisor selects the
matching transport engine, and disconnect/reconnect recovery restores the
previously selected rate.

See `analysis/dynamic-rate-switching-success.md` for the current validated
checkpoint and `analysis/bringup-plan.md` for the original bring-up sequence.
