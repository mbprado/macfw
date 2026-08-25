# macfw 0.01.000 — Alpha

`0.01.000` is the first installable macfw development release for the **M-Audio FireWire 410**.

This release is intended for testing on Intel Macs. It establishes the first complete path from a persistent CoreAudio device through a recoverable user-space FireWire transport to real FW410 hardware.

## Highlights

- Native 44.1 kHz and 48 kHz full-duplex CoreAudio audio.
- 10 playback channels: Analog Out 1-8 and S/PDIF L/R.
- 4 capture channels: Analog In 1-2 and S/PDIF L/R.
- Simultaneous playback, recording and monitoring validated in Logic Pro.
- Runtime 44.1/48 kHz switching.
- Automatic FW410 bootloader handling.
- Automatic physical disconnect/reconnect recovery.
- Persistent CoreAudio endpoint: applications keep the selected FW410 while transport is offline.
- Capture returns silence during an outage and resumes real input after recovery.
- launchd-managed transport service starts automatically at boot and restarts if killed.
- The system can boot without the FW410 and recover automatically when it is connected later.
- `.pkg` installation is hardware-gated to a connected supported FW410.
- Package installation has been validated to make the interface operational immediately without requiring a reboot.

## Installation

See [`INSTALL.md`](INSTALL.md).

The normal binary installation is the macOS `.pkg`. A source build/install path is also available:

```bash
make
sudo make install
```

## Compatibility

Current release target:

- Intel Mac;
- macOS Sonoma or newer;
- M-Audio FireWire 410;
- 44.1 kHz and 48 kHz native audio.

Apple Silicon is not currently supported.

## Alpha notice

This is early compatibility software built through reverse engineering and real-hardware validation. It should not yet be treated as a production replacement for a commercially supported audio driver.

The current validation is strong on the development system but the compatibility matrix across Intel Mac models, FireWire adapters, macOS releases, and FW410 hardware revisions is still small.

Read [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md) before installing.

## Signing/notarization

The release artifact's signing/notarization status must be stated on the GitHub Release when published. Signing/notarization is being treated separately from functional package validation and must not be implied when absent.

## Diagnostics for testers

When reporting a transport problem, include the relevant portion of:

```text
/Library/Logs/macfw-fw410-transport.log
```

and, when using a source checkout:

```bash
fw410/tools/transport/transportstatus/transportstatus
```

Please also identify the Mac model, macOS version, FireWire connection/adapters, requested sample rate, and the event that preceded the failure.

## Detailed changes

See [`CHANGELOG.md`](CHANGELOG.md).
