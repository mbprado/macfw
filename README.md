# macfw

Modern FireWire audio support for macOS.

`macfw` is an open-source reverse-engineering and compatibility project focused on bringing legacy IEEE 1394 / FireWire audio interfaces back to life on modern macOS systems.

The first supported target is the **M-Audio FireWire 410**. The repository is structured so reusable FireWire/audio components can eventually support additional devices and families.

## Current status

The FW410 implementation is now on its **third installable alpha release line (`0.03.000`)** for Intel macOS. Native audio, recovery, low-latency scheduling, runtime rate switching and the current control-panel release scope are hardware-validated on real FW410 hardware.

Hardware-validated functionality includes:

- normal CoreAudio device integration;
- native 44.1 kHz and 48 kHz full-duplex audio;
- 10 playback channels: Analog Out 1-8 and S/PDIF L/R;
- 4 capture channels: Analog In 1-2 and S/PDIF L/R;
- simultaneous playback, recording and software monitoring in Logic Pro;
- dedicated Mach-paced real-time audio service thread at both rates;
- `THREAD_TIME_CONSTRAINT_POLICY` scheduling;
- low-latency 256-frame capture prefill;
- runtime 44.1 <-> 48 kHz switching from Audio MIDI Setup and the native Device tab;
- automatic FW410 bootloader handling;
- physical disconnect/reconnect recovery;
- persistent CoreAudio endpoint with capture silence while transport is offline;
- automatic launchd-managed transport startup/restart;
- reboot recovery and delayed hardware attachment after macOS has already booted;
- native AppKit control panel using the transport-owned control socket;
- headphone source, level and five-pair mixer routing controls;
- AUX source/output level controls;
- physical output Mixer/AUX source selection and independent stereo output levels;
- FW410 main-mixer 7x5 routing assignments for analog input, S/PDIF input and all five software-return pairs;
- simultaneous main-mixer assignments to multiple destination buses;
- live Analog In 1/2 and S/PDIF L/R meters in the Inputs tab;
- Device-tab transport/rate/CoreAudio diagnostics and 44.1/48 kHz selection;
- Info/Diagnostics with exact GUI/HAL/runtime build identity, Copy Diagnostics and Open Transport Log;
- persistent writable control state across reboot and physical interface reconnect;
- safe **Reset Defaults** action for restoring the documented macfw control baseline;
- source and `.pkg` installation paths that include the control-panel application;
- hardware-validated `.pkg` installation, runtime startup, persistence, reset and reconnect lifecycle.

The FW410 requires extra startup work at 44.1 kHz, so **48 -> 44.1 kHz switching is slower than 44.1 -> 48 kHz**. The slower direction is hardware-validated as reliable in the current release candidate.

Current cumulative macOS hardware-test status:

- **Monterey 12.7.6:** validated;
- **Ventura 13.7.8:** validated;
- **Sonoma 14.8.9:** validated, including sleep/wake recovery;
- **Sequoia 15.x:** validated.

Apple Silicon is not currently supported. See [`COMPATIBILITY.md`](COMPATIBILITY.md) and [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md) for the evidence-based compatibility status.

## Install

For binary [`releases`](https://github.com/mbprado/macfw/releases), use the provided macOS `.pkg` with the M-Audio FireWire 410 connected and powered on.

For a source checkout, install the Command Line Tools if needed:

```bash
xcode-select --install
```

Then clone, build as a normal user, and install the already-built artifacts as root:

```bash
git clone https://github.com/mbprado/macfw.git
cd macfw
make
sudo make install
```

The normal build produces the HAL plug-in, release runtime tools and native control panel. `sudo make install` installs those existing artifacts; it does not intentionally rebuild them as root.

Installed user-facing components include:

```text
/Applications/macfw FW410 Control.app
/Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
/Library/Application Support/macfw/fw410/
```

Full instructions, status checks, troubleshooting and uninstall information are in [`INSTALL.md`](INSTALL.md).

## Useful build targets

From the repository root:

```bash
make             # HAL + release runtime + GUI
make hal         # HAL only
make runtime     # release runtime/control tools only
make gui         # control panel only
make all-tools   # development/reverse-engineering tools
make package     # fresh release build + complete installer package
make clean
```

`make runtime` builds only the binaries required by the installed service and control path.

## Build a package

From the repository root:

```bash
make package
```

The package target performs a fresh release-artifact rebuild, then packages the HAL plug-in, launchd/runtime files, persistent control-state helper, runtime build metadata and native control panel under:

```text
package/dist/
```

## Architecture

The project does not port the original M-Audio kernel extension. The current architecture separates the persistent CoreAudio-facing endpoint from a recoverable user-space FireWire transport:

```text
CoreAudio application
        |
macfw AudioServerPlugIn
        |
versioned shared-memory audio/status ABI
        |
macfw transport supervisor / native engine
        |
AV/C / CMP / CIP / AMDTP / NuDCL / IOFireWireLib
        |
M-Audio FireWire 410
```

The native engines keep FireWire ownership inside the transport while separating real-time service work:

```text
normal/control thread
    -> FireWire callbacks / FCP / control IPC

dedicated isoch callback thread
    -> CFRunLoop

dedicated audio service thread
    -> playback / capture / TX service / meter accumulation
    -> Mach pacing + time-constraint scheduling
```

Live hardware controls use the same transport ownership model:

```text
macfw FW410 Control.app / fw410ctl
        |
/tmp/macfw-fw410-control.sock
        |
active transport process
        |
FW410 AV/C controls
```

The GUI and CLI never open FireWire independently. This avoids competing FireWire owners while playback/capture are active.

Writable controls are recorded in `/Library/Application Support/macfw/fw410/control-state.conf`. After native-engine low-level readiness, the transport supervisor restores saved state. Main-mixer routes are restored through the same safe full-baseline-then-differential control path used during normal operation.

The logical CoreAudio device can remain registered while the physical transport is absent. During recovery, playback is discarded and capture supplies silence; when the FW410 returns `ONLINE`, existing CoreAudio clients continue using the same endpoint.

## Control panel

The current AppKit control panel includes:

- **Mixer** — 7-source x 5-destination routing matrix;
- **Outputs** — five physical output pairs with Mixer/AUX selection, L/R levels and stereo link;
- **Headphones** — source, L/R volume and five mixer-output pair enables;
- **AUX** — software-return/AUX output levels;
- **Inputs** — live four-channel capture meters;
- **Device** — transport/CoreAudio state and 44.1/48 kHz selection;
- **Info** — component/runtime identity and support diagnostics.

The Device sample-rate selector uses the normal CoreAudio nominal-sample-rate property and HAL configuration-change lifecycle. It does not bypass CoreAudio or call FireWire rate-control probes directly.

## Main mixer discovery

The original M-Audio panel and the Linux `snd-firewire-ctl-services` implementation established that the FW410 main mixer is a **7-source x 5-destination assignment matrix**:

- sources: Analog In 1/2, S/PDIF In L/R and five software-return pairs;
- destinations: mixer buses 1/2, 3/4, 5/6, 7/8 and S/PDIF;
- one source can feed multiple buses simultaneously;
- enabled cell value is `0x0000`, disabled is `0x8000`.

A crucial hardware quirk is documented and implemented: isolated mixer writes against an unknown state can disrupt the audio path. macfw therefore first establishes a complete known 35-cell matrix with CONTROL writes, keeps that matrix cached in the transport, and performs later route changes differentially without using mixer STATUS polling.

Because macfw's AMDTP slot ordering differs from the original logical software-return ordering, the control-panel GUI remaps the raw FW410 return identities so the user sees the same channel numbering as CoreAudio/Logic.

Detailed evidence is in [`fw410/analysis/original-control-panel-mixer-model.md`](fw410/analysis/original-control-panel-mixer-model.md) and [`fw410/analysis/control-state-persistence.md`](fw410/analysis/control-state-persistence.md).

## Project goals

- Support legacy FireWire audio interfaces on modern macOS.
- Prefer user-space and modern macOS integration over obsolete kernel extensions.
- Separate reusable IEEE 1394/audio functionality from device-specific behavior.
- Reverse engineer vendor protocols where necessary for hardware compatibility.
- Preserve evidence and clearly distinguish confirmed, observed, inferred and unknown behavior.
- Build reusable transport/audio components that can support additional interfaces in the future.

## Repository structure

```text
macfw/
├── README.md
├── INSTALL.md
├── CHANGELOG.md
├── COMPATIBILITY.md
├── KNOWN-LIMITATIONS.md
├── RELEASE-NOTES.md
├── RELEASES.md
├── Makefile
├── package/
│   ├── build-pkg.sh
│   └── scripts/
└── fw410/
    ├── README.md
    ├── Makefile
    ├── hal/
    ├── control-panel/
    ├── lib/
    ├── service/
    ├── tools/
    ├── analysis/
    ├── hardware/
    ├── protocol/
    ├── reference/
    ├── captures/
    ├── experiments/
    └── tests/
```

As reusable components mature, common functionality can be promoted into shared project-level modules.

## Development principles

### User space first

Avoid kernel extensions whenever technically possible. Kernel-level components are not the primary architecture or goal.

### Preserve evidence

Original vendor binaries and other research material should remain immutable and clearly separated from new implementation code.

### Document discoveries

Important reverse-engineering results are recorded as they are established and hardware-tested.

### Separate facts from assumptions

Protocol documentation distinguishes between:

- **Confirmed** — verified by code analysis or hardware testing.
- **Observed** — seen in captures/runtime behavior but not fully explained.
- **Inferred** — strongly indicated by multiple sources.
- **Unknown** — still requiring investigation.

### Prefer protocols over legacy implementation details

The objective is hardware compatibility, not reproducing the architecture of an obsolete vendor driver.

## Current roadmap

Major completed areas now include:

1. Device/firmware identification and guarded boot recovery.
2. User-space FireWire async/FCP/AV/C access.
3. CMP/IRM and NuDCL isochronous transport.
4. CIP/AM824 playback and capture.
5. Native 44.1/48 kHz full-duplex transport.
6. CoreAudio integration and persistent recovery model.
7. launchd service lifecycle and package/source installation.
8. Headphone, AUX and physical-output controls.
9. Hardware-validated 7x5 main-mixer routing with CoreAudio-aligned GUI labels.
10. Persistent hardware/control state, reconnect restore and Reset Defaults.
11. Low-latency 256-frame capture baseline and real-time scheduling at both rates.
12. Live input meters and Inputs tab.
13. Device-tab sample-rate control and CoreAudio diagnostics.
14. Info/Diagnostics release support surface.
15. Hardware-validated end-user build/install/package lifecycle.

Deferred work includes unresolved mixer strip level/pan/mute/AUX-send semantics, calibrated CoreAudio latency reporting, MIDI, named presets, optional menu-bar controls, broader hardware coverage, and signing/notarization.

## Release documentation

- [`INSTALL.md`](INSTALL.md) — install, build, status, troubleshooting and uninstall.
- [`CHANGELOG.md`](CHANGELOG.md) — user-visible changes.
- [`COMPATIBILITY.md`](COMPATIBILITY.md) — hardware-tested macOS compatibility matrix.
- [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md) — unsupported/open behavior.
- [`RELEASE-NOTES.md`](RELEASE-NOTES.md) — current alpha release notes.
- [`RELEASES.md`](RELEASES.md) — versioning, tagging and release contract.
- [`fw410/README.md`](fw410/README.md) — detailed FW410 engineering status.

## Contributing and testing

Useful contributions include hardware testing, FireWire captures, protocol/firmware analysis, macOS/CoreAudio development, and testing across different Intel Macs, macOS versions, adapters and FW410 revisions.

When reporting runtime problems, use **Copy Diagnostics** from the control panel when available and include the system/connection details described in [`INSTALL.md`](INSTALL.md).

## Disclaimer

`macfw` is an independent reverse-engineering and compatibility project. It is not affiliated with, endorsed by, or supported by M-Audio, Avid, Apple, or any hardware manufacturer.

Vendor drivers, firmware and other proprietary material remain subject to their respective licenses and copyrights.

## License

The project license applies to original code and documentation in this repository. Third-party source code, vendor binaries, firmware and other external material remain subject to their respective licenses and copyrights.
