# M-Audio FireWire 1814

The FW1814 is the second macfw device target.

## Current scope

The experimental FW1814 profile currently provides:

- Intel macOS support through Apple's legacy FireWire stack;
- hardware-validated analog full-duplex CoreAudio at 44.1 and 48 kHz;
- Analog Outputs 1-4 and Analog Inputs 1-8;
- rate switching from Audio MIDI Setup;
- automatic boot, transport restart and disconnect/reconnect recovery;
- restoration of the previously selected rate after reconnect;
- a transport-owned, read-only routing-control API for the future GUI.

S/PDIF, ADAT, 88.2/96/176.4/192 kHz, writable routing controls and the native control panel remain under development. MIDI is intentionally deferred until the audio/control surface is complete.

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

## Build and install

From the repository root:

```bash
make fw1814
sudo make fw1814-install
```

Build every FW1814 reverse-engineering/diagnostic tool with `make fw1814-tools`. Full requirements, target descriptions, installation and uninstall instructions are centralized in [`../../INSTALL.md`](../../INSTALL.md).

## Routing control API

The active transport owns `/tmp/macfw-fw1814-control.sock`; clients never open FireWire independently. The initial `fw1814ctl` surface is deliberately read-only because the special-firmware mixer registers cannot be queried safely:

```bash
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" routing get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" capabilities get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" engine get
```

This establishes the backend boundary for the future GUI. Writable controls will be enabled only as individual documented register values are hardware-validated.
