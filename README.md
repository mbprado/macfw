# macfw

Modern FireWire audio support for macOS.

`macfw` is an open-source reverse-engineering and compatibility project focused on bringing legacy IEEE 1394 / FireWire audio interfaces back to life on modern macOS systems.

The first supported target is the **M-Audio FireWire 410**. The repository is structured so reusable FireWire/audio components can eventually support additional devices and families.

## Current status

The FW410 implementation has reached its **first installable alpha candidate** on Intel macOS.

Hardware-validated functionality includes:

- normal CoreAudio device integration;
- native 44.1 kHz and 48 kHz full-duplex audio;
- 10 playback channels: Analog Out 1-8 and S/PDIF L/R;
- 4 capture channels: Analog In 1-2 and S/PDIF L/R;
- simultaneous playback, recording and monitoring in Logic Pro;
- runtime 44.1 <-> 48 kHz switching;
- automatic FW410 bootloader handling;
- physical disconnect/reconnect recovery;
- persistent CoreAudio endpoint with capture silence while transport is offline;
- automatic launchd-managed transport startup/restart;
- reboot recovery and delayed hardware attachment after macOS has already booted;
- native `.pkg` installation, including a clean-system Monterey 12.7.6 install that became operational as expected.

Current macOS test status:

- **Monterey 12.7.6:** validated on a fresh install, with playback/capture and package installation working as expected;
- **Ventura 13.x:** not yet tested;
- **Sonoma 14.8.9:** installer, playback and transport behavior functioned, but capture quality was audibly degraded in the quick functional test and needs follow-up.

Apple Silicon is not currently supported. See [`COMPATIBILITY.md`](COMPATIBILITY.md) and [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md) for the current evidence-based compatibility status.

## Install

For binary releases, use the provided macOS `.pkg` with the M-Audio FireWire 410 connected and powered on.

For a source checkout:

```bash
make
sudo make install
```

Full instructions, status checks, troubleshooting and uninstall information are in [`INSTALL.md`](INSTALL.md).

## Build a package

From the repository root:

```bash
make package
```

The resulting package is written to:

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
macfw transport supervisor
        |
AV/C / CMP / CIP / AMDTP / NuDCL / IOFireWireLib
        |
M-Audio FireWire 410
```

The logical CoreAudio device can remain registered while the physical transport is absent. During recovery, playback is discarded and capture supplies silence; when the FW410 returns `ONLINE`, existing CoreAudio clients continue using the same endpoint.

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
├── package/                 # root macOS package builder/output
│   ├── build-pkg.sh
│   └── scripts/
└── fw410/
    ├── README.md
    ├── Makefile
    ├── hal/                 # CoreAudio AudioServerPlugIn
    ├── lib/                 # reusable FW410/FireWire implementation
    ├── service/             # launchd runtime installation
    ├── tools/               # control/device/transport tools
    ├── analysis/            # validated engineering documentation
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

Completed for the first FW410 alpha candidate:

1. Device/firmware identification and boot recovery.
2. User-space FireWire async/FCP/AV/C access.
3. CMP/IRM and NuDCL isochronous transport.
4. CIP/AM824 playback and capture.
5. Native 44.1/48 kHz full-duplex transport.
6. CoreAudio integration.
7. Persistent transport-status/offline model.
8. Automatic disconnect/reconnect recovery.
9. launchd service lifecycle.
10. Source and `.pkg` installation paths.

Next areas include broader hardware/macOS validation, latency tuning, mixer/controls, MIDI, release automation, signing/notarization, and eventually additional FireWire devices.

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

When reporting runtime problems, include the system/connection details and relevant transport logs described in [`INSTALL.md`](INSTALL.md).

## Disclaimer

`macfw` is an independent reverse-engineering and compatibility project. It is not affiliated with, endorsed by, or supported by M-Audio, Avid, Apple, or any hardware manufacturer.

Vendor drivers, firmware and other proprietary material remain subject to their respective licenses and copyrights.

## License

The project license applies to original code and documentation in this repository. Third-party source code, vendor binaries, firmware and other external material remain subject to their respective licenses and copyrights.
