# macfw 0.02.000 — Alpha

`0.02.000` is the second installable macfw development release for the **M-Audio FireWire 410**.

This release builds on the `0.01.000` CoreAudio/transport/recovery foundation and adds the first substantial native hardware-control surface, persistent writable device state and a hardware-validated end-user package lifecycle.

## Highlights

- Native AppKit **macfw FW410 Control** application installed in `/Applications`.
- Hardware-validated FW410 7-source x 5-bus main-mixer routing.
- Main-mixer routing for Analog In 1/2, S/PDIF input and all five software-return pairs.
- Multiple simultaneous mixer-bus assignments.
- CoreAudio/Logic-aligned software-return names in the Mixer GUI.
- Physical output Mixer/AUX source selection and independent stereo levels.
- Headphone source, stereo level and five-pair mixer routing.
- AUX stream/output level controls.
- Persistent writable hardware-control state across reboot and physical FW410 disconnect/reconnect.
- **Reset Defaults** action for restoring and recording the documented macfw control baseline.
- Safe main-mixer initialization using a complete known 35-cell matrix before differential route writes.
- Dedicated package postinstall diagnostics in `/Library/Logs/macfw_install.log`.
- Package bundle relocation disabled so the control application remains at `/Applications/macfw FW410 Control.app`.
- Complete `.pkg` path hardware-validated through installation, service startup, status, controls, reset, reboot persistence and reconnect persistence.

The underlying audio/recovery behavior from `0.01.000` remains present:

- native 44.1 kHz and 48 kHz full-duplex CoreAudio audio;
- 10 playback channels: Analog Out 1-8 and S/PDIF L/R;
- 4 capture channels: Analog In 1-2 and S/PDIF L/R;
- simultaneous playback, recording and monitoring validated in Logic Pro;
- runtime 44.1/48 kHz switching;
- automatic FW410 bootloader handling;
- automatic physical disconnect/reconnect recovery;
- persistent CoreAudio endpoint while transport is offline;
- launchd-managed transport startup/restart;
- boot without the FW410 followed by delayed automatic recovery when it is connected later.

## Control architecture

Audio and controls continue to share one FireWire owner:

```text
macfw FW410 Control.app / fw410ctl
        |
/tmp/macfw-fw410-control.sock
        |
active transport engine
        |
FW410 AV/C
```

The GUI and CLI do not open FireWire independently while playback/capture are active.

The main mixer has a validated device-specific initialization rule: macfw first writes a complete known 35-cell matrix, caches it in the transport, and only then performs individual route changes. Mixer STATUS polling is avoided.

Successful writable control changes are recorded by `fw410state`. On startup/reconnect, saved controls are restored after native-engine low-level readiness and before the supervisor publishes `ONLINE`. Saved mixer routes are replayed through the same safe full-baseline-first path.

## Installation

See [`INSTALL.md`](INSTALL.md).

The normal binary installation is the macOS `.pkg`. With the FW410 connected and powered on, installation is hardware-gated to a supported device personality and installs:

```text
/Applications/macfw FW410 Control.app
/Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
/Library/Application Support/macfw/fw410/
/Library/LaunchDaemons/com.mbprado.macfw.fw410.transport.plist
```

A source build/install path remains available:

```bash
make
sudo make install
```

The default source build creates the HAL plug-in, release runtime and native GUI. Installation uses those already-built artifacts rather than intentionally compiling as root.

## Persistent controls

The installed state helper is:

```text
/Library/Application Support/macfw/fw410/tools/control/fw410state/fw410state
```

Saved state is stored in:

```text
/Library/Application Support/macfw/fw410/control-state.conf
```

Persistence currently covers all writable production controls exposed through the normal CLI/GUI path. The control-panel **Reset Defaults** action applies the documented macfw baseline; it is not presented as an undocumented M-Audio factory reset.

The persistence and reconnect-restore lifecycle has been hardware-validated after both reboot and physical FW410 disconnect/reconnect.

## Compatibility

Hardware validation currently includes:

- Monterey 12.7.6 on Intel Mac;
- Ventura 13.7.8 on Intel Mac;
- Sonoma 14.8.9 on Intel Mac.

Supported device/rates:

- M-Audio FireWire 410;
- native 44.1 kHz and 48 kHz audio.

Apple Silicon is not currently supported.

See [`COMPATIBILITY.md`](COMPATIBILITY.md) for the detailed tested matrix.

## Known limitations

The complete original M-Audio mixer/control surface is not yet implemented. Remaining control work includes strip-level/pan/mute/solo/AUX-send semantics where those controls are safely confirmed.

Capture still uses a conservative 4,096-frame prefill, so low-latency software monitoring has not yet been optimized. MIDI is not yet a validated user-facing feature. S/PDIF-specific control validation is less extensive than the analog-output testing.

Read [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md) for the complete list.

## Signing/notarization

`0.02.000` remains an alpha release. Unless explicitly stated otherwise on the GitHub Release, the package should be treated as **unsigned and unnotarized**. Signing/notarization is being handled separately from functional package validation and is not implied by successful installation/testing.

## Diagnostics for testers

For runtime/control issues, include the relevant portion of:

```text
/Library/Logs/macfw-fw410-transport.log
```

For package postinstall problems, inspect:

```text
/Library/Logs/macfw_install.log
```

Installer-level failures that occur before macfw's postinstall starts may also require `/var/log/install.log`.

When using a source checkout, transport state can be inspected with:

```bash
fw410/tools/transport/transportstatus/transportstatus
```

Please also identify the Mac model, macOS version, FireWire connection/adapters, requested sample rate, and the event that preceded the failure.

## Detailed changes

See [`CHANGELOG.md`](CHANGELOG.md).
