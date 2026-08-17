# FW410 HAL AudioServerPlugIn

This is the active CoreAudio integration path while DriverKit provisioning is unavailable.

## Milestone 1

Publish a real CoreAudio device named `M-Audio FireWire 410` with:

- 2 output channels;
- 44.1 kHz and 48 kHz nominal rates;
- Float32 interleaved CoreAudio stream format;
- synthetic host clock/timestamps;
- output currently discarded (silence).

Apple documents AudioServerPlugIns as `.driver` bundles loaded by CoreAudio from `/Library/Audio/Plug-Ins/HAL`. The plug-in may communicate with another process through declared Mach services, which is how the next milestone will connect to the existing user-space `IOFireWireLib` transport service.

## Build

```bash
cd fw410/hal
make clean
make
```

The result is `build/macfw-fw410.driver`.

## Install for the enumeration test

```bash
sudo make install
sudo killall coreaudiod
```

Then open Audio MIDI Setup and look for `M-Audio FireWire 410`.

To remove it:

```bash
sudo make uninstall
sudo killall coreaudiod
```

No DriverKit entitlement or provisioning profile is involved in this path.

## Architecture after enumeration

```text
CoreAudio HAL
    |
macfw-fw410.driver
    |
Mach IPC + shared PCM ring
    |
macfw FW410 transport service
    |
IOFireWireLib / CMP / AMDTP
    |
M-Audio FireWire 410
```

The HAL plug-in must remain small and realtime-safe. FireWire discovery, AV/C, CMP, the 44.1 kHz M-Audio startup quirk, NuDCL programs and bus-reset recovery remain in the companion transport service.
