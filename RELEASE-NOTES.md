# macfw 0.4.000 — Alpha

`0.4.000` is the first unified macfw development release for the **M-Audio
FireWire 410** and **M-Audio FireWire 1814**.

Both interfaces now share one project version and one primary installer while
retaining independent drivers, transport services, control state and native
control-panel applications.

## Highlights

- One `macfw-0.4.000-<build>.pkg` installs support for both interfaces.
- Root `make` builds the FW410 and FW1814 HAL, release runtime and control panel.
- `sudo make install` preflights every required artifact, then installs both interfaces.
- Device-specific build, install and package targets remain available.
- FW1814 reaches its first installable control-panel release scope.
- FW410 retains the hardware-validated `0.03.000` audio/control baseline.

## FW1814 release scope

The FW1814 implementation provides hardware-validated analog full-duplex
CoreAudio operation at 44.1 and 48 kHz, exposing Analog Outputs 1–4 and Analog
Inputs 1–8. The current native AppKit control panel covers:

- software-return mixer levels;
- analog-input monitor level and pan;
- analog-output source and volume;
- both digital-volume headphone outputs;
- AUX sends and AUX master volume;
- continuous linked or independent stereo-slider updates;
- persistent control restoration across restart, rate switch and reconnect.

The runtime also provides automatic bootloader recovery, launchd supervision,
sample-rate switching through CoreAudio and physical disconnect/reconnect
recovery. S/PDIF, ADAT, higher sample rates and MIDI remain deferred.

## FW410 retained baseline

The existing FW410 release functionality remains included:

- native 44.1/48 kHz full-duplex CoreAudio audio;
- 10 playback and 4 capture channels;
- dedicated Mach-paced real-time audio servicing and 256-frame capture prefill;
- control-panel and Audio MIDI Setup rate switching;
- bootloader, reboot, delayed-attachment and disconnect/reconnect recovery;
- main mixer, physical output, headphone and AUX controls;
- live input meters, Device diagnostics and Info/Diagnostics;
- persistent control state and Reset Defaults.

The FW410 still requires extra startup work at 44.1 kHz, so 48 -> 44.1 kHz
switching is slower than the reverse direction.

## Build and installation

Build and install both interfaces from source:

```bash
make
sudo make install
```

Compilation runs as the normal user. Aggregate installation checks that both
device builds are complete before installing either one.

Build the unified installer:

```bash
make package
```

The output is:

```text
package/dist/macfw-0.4.000-<build>.pkg
```

The combined hardware gate accepts either a connected FW410 or FW1814 and then
installs both namespaced device stacks. Focused installers remain available via
`make fw410-package` and `make fw1814-package`; their gates still require the
matching interface.

Installed user-facing applications are:

```text
/Applications/macfw FW410 Control.app
/Applications/macfw FW1814 Control.app
```

See [`INSTALL.md`](INSTALL.md) for complete build, installation, status,
troubleshooting and uninstall instructions.

## Compatibility

The current architecture targets Intel Macs using Apple's legacy FireWire
stack. Cumulative hardware validation includes Monterey 12.7.6, Ventura
13.7.8, Sonoma 14.8.9 and Sequoia 15.x.

Apple Silicon is not currently supported. macOS Tahoe 26 is unsupported because
Apple removed the built-in FireWire stack used by macfw. See
[`COMPATIBILITY.md`](COMPATIBILITY.md) and
[`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md).

## Signing and notarization

`0.4.000` remains an alpha release. Unless explicitly stated otherwise on the
GitHub Release, the package is **unsigned and unnotarized**.

## Diagnostics for testers

Use **Copy Diagnostics** in the appropriate control panel first. Transport logs
are stored at:

```text
/Library/Logs/macfw-fw410-transport.log
/Library/Logs/macfw-fw1814-transport.log
```

Package postinstall activity is recorded in:

```text
/Library/Logs/macfw_install.log
```

When reporting a problem, include the interface model, Mac model, macOS
version, FireWire adapters, sample rate and the event that preceded the failure.

## Detailed changes

See [`CHANGELOG.md`](CHANGELOG.md).
