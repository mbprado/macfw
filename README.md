# macfw

Modern FireWire support for macOS.

`macfw` is an open-source research and development project focused on bringing legacy IEEE 1394 / FireWire audio interfaces back to life on modern macOS systems.

The project starts with the **M-Audio FireWire 410**, but the repository is designed from the beginning to support additional FireWire audio interfaces and device families.

## Goals

- Support legacy FireWire audio interfaces on modern macOS.
- Initially target **Intel Macs** and **macOS Sonoma and newer**.
- Reverse engineer existing vendor drivers and hardware protocols where necessary.
- Separate common FireWire functionality from device-specific implementations.
- Prefer modern macOS driver architectures and minimize privileged code.
- Build reusable protocol knowledge that can benefit multiple FireWire devices.

Apple Silicon is not currently a project target.

## Repository structure

Each supported interface gets its own device directory:

```text
macfw/
├── README.md
│
├── fw410/
│   ├── README.md
│   ├── original/
│   ├── analysis/
│   ├── hardware/
│   ├── protocol/
│   ├── reference/
│   ├── captures/
│   ├── experiments/
│   ├── driver/
│   ├── tools/
│   └── tests/
│
└── <future-device>/
```

This organization intentionally keeps device-specific reverse engineering separate while leaving room for a common FireWire layer to emerge as the project develops.

## Current target: M-Audio FireWire 410

The first device under investigation is the **M-Audio FireWire 410 (FW410)**, a BeBoB-based FireWire audio interface.

See [`fw410/README.md`](fw410/README.md) for the device-specific project and [`fw410/analysis/`](fw410/analysis/) for reverse-engineering work.

## Development approach

The project is deliberately divided into stages:

```text
Vendor driver / hardware
          │
          ▼
   Reverse engineering
          │
          ▼
   Protocol analysis
          │
          ├──────────────┐
          ▼              ▼
      Linux           FFADO
      BeBoB          reference
          │              │
          └──────┬───────┘
                 ▼
        Protocol specification
                 │
                 ▼
       Modern macOS transport
                 │
                 ▼
             CoreAudio
```

The objective is **hardware compatibility**, not a line-by-line port of obsolete vendor source code.

## Common vs. device-specific functionality

A major architectural goal is to identify which functionality can eventually be shared between devices:

- IEEE 1394 transport
- asynchronous transactions
- isochronous streaming
- CIP handling
- AVC commands
- bus-reset handling
- common BeBoB functionality
- CoreAudio integration

Device-specific code will remain under the corresponding device directory, for example:

```text
fw410/protocol/
```

until there is enough evidence that a component belongs in a shared layer.

## Current status

**Research / reverse engineering**

No functional modern macOS driver is available yet.

The immediate priority is to understand the FW410's original driver, firmware behavior, FireWire transport, and device protocol before committing to a final Sonoma architecture.

## Development principles

### Preserve evidence

Original vendor binaries and other research material should be kept immutable and clearly separated from new implementation code.

### Document discoveries

Important reverse-engineering results should be recorded in the repository as they are established.

### Separate facts from assumptions

Protocol documentation should distinguish between:

- **Confirmed** — verified by code analysis or hardware testing.
- **Observed** — seen in captures or runtime behavior but not fully explained.
- **Inferred** — strongly indicated by multiple sources.
- **Unknown** — still requiring investigation.

### Prefer protocols over implementations

The goal is to understand what the hardware requires, not to reproduce the architecture of an obsolete vendor kext.

### Minimize privileged code

Modern macOS compatibility should use the smallest possible privileged component and prefer user-space / DriverKit mechanisms where technically possible.

## Project roadmap

1. Repository and evidence collection
2. Device and firmware identification
3. Original driver analysis
4. FireWire and BeBoB protocol reconstruction
5. Comparison with Linux and FFADO
6. Hardware traffic capture and validation
7. Modern macOS transport prototype
8. CoreAudio integration
9. Device-specific controls and MIDI
10. Generalize reusable components for additional FireWire devices

## Contributing

Useful contributions include:

- Hardware testing
- FireWire traffic captures
- Firmware analysis
- Protocol documentation
- Reverse engineering
- Linux / FFADO research
- DriverKit and AudioDriverKit development
- CoreAudio development
- Testing on different Intel Macs and macOS versions
- Testing different hardware and firmware revisions

## Disclaimer

`macfw` is an independent reverse-engineering and compatibility project. It is not affiliated with, endorsed by, or supported by M-Audio, Avid, Apple, or any hardware manufacturer.

Vendor drivers, firmware and other proprietary material remain subject to their respective licenses and copyrights.

## License

The project license applies to original code and documentation in this repository. Third-party source code, vendor binaries, firmware and other external material remain subject to their respective licenses and copyrights.
