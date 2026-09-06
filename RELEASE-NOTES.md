# macfw 0.03.000 — Alpha

`0.03.000` is the third installable macfw development release for the **M-Audio FireWire 410**.

This release focuses on real-time audio quality, lower software-monitoring latency, completion of the current control-panel release scope, and a more reliable 44.1/48 kHz runtime lifecycle.

## Highlights

- Hardware-validated low-latency full-duplex operation at **44.1 kHz and 48 kHz**.
- Dedicated Mach-paced real-time audio service thread at both supported rates.
- `THREAD_TIME_CONSTRAINT_POLICY` scheduling for the audio service path.
- Capture prefill reduced from the earlier 4,096-frame development baseline to **256 frames**.
- Excellent subjective round-trip software-monitoring latency in the validated physical-loopback/Logic test path.
- New **Inputs** tab with live Analog In 1/2 and S/PDIF L/R meters.
- New **Device** tab with active/requested sample rate, transport state, engine PID and CoreAudio buffer diagnostics.
- 44.1/48 kHz selection directly from the control panel through the normal CoreAudio/HAL device-configuration lifecycle.
- Expanded **Info** tab with exact GUI/HAL/runtime build identity, transport diagnostics, **Copy Diagnostics** and **Open Transport Log**.
- Runtime build metadata persisted by both source installation and packaged installation.
- Control-panel build cleaned up so the app is linked once without the previous Makefile target-splitting warnings.
- `make`, `sudo make install`, and `make package` release paths corrected and revalidated.

## Audio/runtime improvements

The native 44.1 kHz and 48 kHz engines now use the same release scheduling model:

```text
normal/control thread
    -> FireWire callbacks / FCP / local control IPC

dedicated isoch callback thread
    -> CFRunLoop
    -> USER_INTERACTIVE QoS

dedicated audio service thread
    -> playback + capture + TX servicing + meter accumulation
    -> USER_INTERACTIVE QoS
    -> 250 us Mach pacing
    -> THREAD_TIME_CONSTRAINT_POLICY
```

Hardware testing showed a major reduction in cutoffs at 44.1 kHz while preserving very low perceived round-trip latency. The same architecture was then validated at 48 kHz, where perceived latency was slightly lower again.

Capture now uses a **256-frame prefill**:

```text
44.1 kHz: ~5.8 ms
48 kHz:   ~5.3 ms
```

This value is one internal buffering component; it is not presented as complete CoreAudio or end-to-end latency.

## Sample-rate switching

Runtime switching remains available through both Audio MIDI Setup and the macfw Device tab.

The FW410 still requires extra device-specific work when starting 44.1 kHz, so **48 -> 44.1 kHz remains noticeably slower than 44.1 -> 48 kHz**. The slower direction is now hardware-validated as reliable and completes consistently.

A release-candidate regression was traced to Unix-socket clients disappearing during the longer 44.1 startup window. A late meter/control reply could raise `SIGPIPE` and terminate the native engine, sending the supervisor into its recovery loop. `0.03.000` hardens the transport against `SIGPIPE` and delays the 44.1 meter listener until the engine has completed its startup/reassert sequence.

## Control panel

The native AppKit control panel now includes:

- **Mixer** — validated 7-source x 5-bus routing matrix;
- **Outputs** — Mixer/AUX source, independent L/R levels and stereo link for five output pairs;
- **Headphones** — source, independent L/R volume, five mixer-output pair enables and stereo link;
- **AUX** — software-return 1/2 and AUX output stereo levels;
- **Inputs** — live Analog Input 1/2 and S/PDIF L/R capture meters;
- **Device** — connection/transport state, sample-rate selection and CoreAudio buffer diagnostics;
- **Info** — component/runtime identity and support diagnostics.

The Device rate selector writes the standard CoreAudio nominal sample-rate property. The GUI does not open FireWire or bypass the HAL/transport lifecycle.

The Info page now provides an exact installed runtime version/build where available and can copy a support snapshot containing transport status and recent runtime log output.

Latency/safety-offset fields are shown as **Not reported by HAL** instead of displaying misleading zero-valued placeholders. Calibrated CoreAudio latency reporting remains future work.

## Existing validated controls

The `0.02.000` control architecture remains intact:

- physical output Mixer/AUX source selection and L/R levels;
- headphone source, L/R level and five-pair mixer routing;
- AUX stream/output levels;
- complete 7-source x 5-bus main-mixer assignment routing;
- multiple simultaneous mixer-bus assignments;
- CoreAudio/Logic-aligned software-return labels;
- persistent writable control state across restart/reboot/reconnect;
- Reset Defaults to the documented macfw baseline.

The main mixer still requires a complete coherent 35-cell initialization before differential route writes. Mixer STATUS polling remains deliberately avoided.

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

Local package creation is:

```bash
make package
```

The package target rebuilds the release artifacts before staging them so embedded build identities remain aligned with the package commit.

## Compatibility

Hardware validation currently includes Intel Macs running:

- Monterey 12.7.6;
- Ventura 13.7.8;
- Sonoma 14.8.9;
- Sequoia 15.x.

Supported device/rates:

- M-Audio FireWire 410;
- native 44.1 kHz and 48 kHz audio.

Apple Silicon is not currently supported.

See [`COMPATIBILITY.md`](COMPATIBILITY.md) for the detailed cumulative test matrix.

## Known limitations

- 48 -> 44.1 kHz switching is slower than the reverse direction because of the FW410-specific 44.1 startup sequence.
- CoreAudio latency/safety-offset properties are not yet calibrated/reported.
- Main-mixer strip level, pan/balance, mute/solo and AUX-send semantics remain intentionally parked until their signal path is hardware-confirmed.
- MIDI is not yet a validated user-facing feature.
- S/PDIF-specific physical validation is less extensive than the analog-output testing.

Read [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md) for the complete list.

## Signing/notarization

`0.03.000` remains an alpha release. Unless explicitly stated otherwise on the GitHub Release, the package should be treated as **unsigned and unnotarized**. Functional package validation does not imply Apple signing/notarization.

## Diagnostics for testers

The control panel's **Copy Diagnostics** action is the preferred first support snapshot.

Transport log:

```text
/Library/Logs/macfw-fw410-transport.log
```

Package postinstall log:

```text
/Library/Logs/macfw_install.log
```

Source-checkout transport status:

```bash
fw410/tools/transport/transportstatus/transportstatus
```

Please also identify the Mac model, macOS version, FireWire connection/adapters, requested sample rate, and the event that preceded the failure.

## Detailed changes

See [`CHANGELOG.md`](CHANGELOG.md).
