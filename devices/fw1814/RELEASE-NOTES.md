# macfw FW1814 0.4.000 — Alpha

`0.4.000` is the first installable macfw development release for the
**M-Audio FireWire 1814**.

## Highlights

- Hardware-validated analog full-duplex CoreAudio operation at 44.1 and 48 kHz.
- Runtime sample-rate switching from Audio MIDI Setup and the native Device tab.
- Automatic bootloader recovery, launchd supervision and physical reconnect.
- Persistent restoration of the selected rate and validated analog controls.
- Native AppKit control panel for software returns, analog input monitoring,
  analog outputs, both digital-volume headphone outputs and the AUX bus.
- Continuous linked or independent stereo level controls and analog-input pan.
- Device-specific source install/uninstall and macOS installer package.
- Independent FW1814 paths allow the FW410 and FW1814 drivers and control
  panels to coexist.
- Hardware detection distinguishes the FW1814 operational identity (`FW 1814`)
  and bootloader identity (`FW 1814 Bootloader`) from the FW410 generic
  `FW Bootloader` identity.

## Current scope

The release exposes Analog Outputs 1–4 and Analog Inputs 1–8. The validated
control surface includes mixer routing, input monitor levels/pan, software
return levels, output source/volume, headphone source/volume, AUX sends and AUX
master volume. Successful changes persist across service restart, rate switch
and reconnect.

S/PDIF, ADAT, higher sample rates and MIDI remain deferred pending direct
comparison with the original M-Audio control panels.

## Installation

The primary `0.4.000` distribution contains both device payloads and installs
only the connected interface stack(s):

```bash
sudo installer -pkg macfw-0.4.000-<build>.pkg -target /
```

If both interfaces are connected, both stacks are installed. To install only
the FW1814 package regardless of FW410 presence, use the device-specific
package:

```bash
sudo installer -pkg macfw-fw1814-0.4.000-<build>.pkg -target /
```

The package installs:

```text
/Applications/macfw FW1814 Control.app
/Library/Audio/Plug-Ins/HAL/macfw-fw1814.driver
/Library/Application Support/macfw/fw1814/
/Library/LaunchDaemons/com.mbprado.macfw.fw1814.transport.plist
```

Source build, package, status and uninstall instructions are in
[`INSTALL.md`](../../INSTALL.md).

## Compatibility

This alpha targets Intel macOS systems using Apple's legacy FireWire stack.
Apple Silicon and macOS Tahoe 26 are not currently supported. See
[`COMPATIBILITY.md`](../../COMPATIBILITY.md) and
[`KNOWN-LIMITATIONS.md`](../../KNOWN-LIMITATIONS.md).
