# macfw 0.05.000 — Alpha

This unified alpha release includes the M-Audio FireWire 410 and FireWire 1814. Both devices retain separate HAL plug-ins, launchd services, saved state and control panels. The combined installer selects the connected model; focused installers are available for each device.

## Changes since 0.04.003

- FW1814 analog playback and capture now run at 44.1, 48, 88.2 and 96 kHz with guarded rolling transmit and live Aggressive (250 µs), Balanced (375 µs) and Conservative (500 µs) service profiles. The 176.4 and 192 kHz analog engines and their profiles are included as experimental modes.
- FW1814 48 kHz capture drops stale input when a client starts and bounds its live queue to 2,048 frames. The control panel waits for the requested engine and restored controls after a GUI rate change, avoiding a transient missing-socket message.
- The combined and FW1814-only packages include all six analog engines and the firmware reset helper. The FW410 implementation retains its previous audio scope; the shared release version is advanced for both devices.

## Hardware observations

On the FW1814 development Mac, playback, recording, repeated rate changes and interface restart worked across the four established modes. At 176.4 and 192 kHz, analog playback/recording and all three live profiles worked; a 176.4 kHz startup occasionally needed its guarded capture-qualification retry. Measured electrical loopback after rolling transmit included approximately 21 ms at 88.2 kHz, 10–15 ms at 96 kHz, 10–12 ms at 176.4 kHz and 9–10 ms at 192 kHz. The 48 kHz capture-backlog validation returned 39/40 impulses at 11.98–16.31 ms in a clean run and 40/40 at 16.48–27.48 ms while browser audio played. The longer loaded returns coincided with receive DBC gaps. These figures are observations on one setup, not guaranteed latency for every client or host.

FW410 behavior and the older macOS compatibility matrix were not retested across every operating system for this candidate. The FW1814 high-rate measurements were made on the development Mac; see [compatibility](COMPATIBILITY.md) and [limitations](KNOWN-LIMITATIONS.md) for the tested scope.

## Install

Build from source as a normal user, then install with administrator privileges:

```bash
make
sudo make install
```

Or install the combined package, which selects connected hardware:

```bash
sudo installer -pkg macfw-0.05.000-<build>.pkg -target /
```

Focused packages are `macfw-fw410-0.05.000-<build>.pkg` and `macfw-fw1814-0.05.000-<build>.pkg`. Build all three locally using `make package`, `make fw410-package` and `make fw1814-package`. Consult [INSTALL.md](INSTALL.md) for requirements, service checks, uninstall and package details.

The native applications install as `/Applications/macfw FW410 Control.app` and `/Applications/macfw FW1814 Control.app`.

## Limitations and reporting

This alpha targets Intel Macs with Apple's legacy FireWire stack. Apple Silicon and macOS Tahoe 26 are unsupported. FW1814 quad-rate startup/capture qualification remains experimental; digital I/O and MIDI remain deferred. Under high host demand, brief FW1814 capture artifacts or DBC gaps may occur. FW1814 HAL device latency values are provisional estimates and do not replace physical loopback measurements.

Packages are unsigned and unnotarized. Report the interface, Mac and macOS version, FireWire adapters, selected rate/profile, preceding action and applicable transport log (`/Library/Logs/macfw-fw410-transport.log` or `/Library/Logs/macfw-fw1814-transport.log`). See [CHANGELOG.md](CHANGELOG.md) for detailed changes.
