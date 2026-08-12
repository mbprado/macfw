# M-Audio FireWire 410 — Modern macOS Driver / DevKit

Reverse-engineering and **user-space driver / DevKit** project for the M-Audio FireWire 410 (FW410).

## Primary goal

The goal is **not** to port the original M-Audio kernel extension.

The goal is to develop a modern **user-space macOS FireWire audio driver / development kit** that can eventually be reused by other legacy FireWire audio interfaces. The FW410 is the first hardware backend used to develop and validate that stack.

```text
                    macfw
                      │
        ┌─────────────┴─────────────┐
        │                           │
        ▼                           ▼
 FireWire / transport          Audio DevKit
        │                           │
        └─────────────┬─────────────┘
                      │
                      ▼
                 FW410 backend
                      │
                      ▼
                CoreAudio / HAL
```

The exact split between ordinary user-space processes, DriverKit, and AudioDriverKit will be determined experimentally rather than assumed.

## Non-goals

- Porting the original kext line-by-line.
- Maintaining a new legacy kernel extension as the primary architecture.
- Reproducing M-Audio's original source architecture.
- Apple Silicon support at this stage.

A kernel component should only be considered if a required low-level capability cannot be implemented through the modern user-space stack.

## Hardware

**Device:** M-Audio FireWire 410  
**Interface:** IEEE 1394 / FireWire  
**Device family:** BeBoB-based FireWire audio device

The FW410 provides FireWire audio streaming, multiple analog inputs/outputs, S/PDIF, MIDI, headphone output, hardware mixer/routing, multiple sample rates, and clock/source configuration.

## Current research question

Before implementing the audio driver, we need to establish the lowest-level FireWire access path available to a user-space application on Intel macOS Sonoma and newer.

Apple provides modern DriverKit and AudioDriverKit frameworks, but there is no public `FireWireDriverKit` family. Apple also documents legacy FireWire device interfaces exposed through IOKit. The project therefore needs to experimentally determine which asynchronous and isochronous FireWire operations remain usable from user space and which parts must be recreated.

This is the first technical milestone.

## Architecture candidates

### A. User-space FireWire service + AudioDriverKit

```text
CoreAudio
   │
   ▼
AudioDriverKit
   │
   │ IPC / shared memory
   ▼
macfw FireWire service
   │
   ▼
IEEE 1394 controller
   │
   ▼
FW410
```

### B. DriverKit-based FireWire stack

```text
CoreAudio
   │
   ▼
AudioDriverKit
   │
   ▼
macfw FireWire stack
   │
   ▼
OHCI / FireWire controller
   │
   ▼
FW410
```

### C. Existing user-space FireWire interfaces

If macOS exposes sufficient functionality through existing IOKit/FireWire device interfaces, those interfaces may be used initially to validate the protocol without building the complete transport stack.

The selected architecture should provide reliable asynchronous and isochronous FireWire access while keeping the audio path in user space.

## Protocol layers

The implementation should eventually separate these layers:

```text
┌───────────────────────────────────────┐
│ CoreAudio / AudioDriverKit            │
├───────────────────────────────────────┤
│ macfw Audio DevKit                    │
│ streams / clock / controls / MIDI     │
├───────────────────────────────────────┤
│ Device protocol                       │
│ FW410 / BeBoB                         │
├───────────────────────────────────────┤
│ FireWire audio transport              │
│ CIP / isochronous / connections       │
├───────────────────────────────────────┤
│ IEEE 1394 transport                   │
│ async transactions / bus management   │
├───────────────────────────────────────┤
│ Hardware                              │
│ OHCI / IEEE 1394 controller           │
└───────────────────────────────────────┘
```

Only the FW410-specific layers should contain M-Audio-specific assumptions.

## Reverse engineering

The original M-Audio driver is treated as immutable reference material.

Initial analysis established that the supplied driver is an **x86_64 legacy kernel extension**, not a 32-bit binary. It contains substantial FireWire, audio, firmware, and FW410-specific implementation code.

Important components discovered so far include:

```text
FWIsochChannel
FWDCLProgram
FWDCLInputProgram
FWDCLOutputProgram
FWP2PConnection
FWConnectionManager
FWAVCConnectionManager
FWAudioDevice
FWAudioEngine
FWAudioStream
FWUserClient
FW410
com_m_audio_FW410Device
```

See [`analysis/`](analysis/) for the reverse-engineering work.

## Development milestones

### M0 — Repository and evidence

- [x] Preserve original driver information
- [x] Identify Mach-O architecture
- [ ] Record hashes of original files
- [ ] Extract and document `Info.plist`
- [ ] Complete initial class/function map

### M1 — User-space FireWire proof of concept

- [ ] Identify available FireWire device interfaces on Sonoma
- [ ] Detect an attached FireWire controller from user space
- [ ] Detect the FW410
- [ ] Read the configuration ROM
- [ ] Enumerate FireWire nodes
- [ ] Perform a harmless asynchronous read
- [ ] Perform a harmless asynchronous write if appropriate
- [ ] Determine whether isochronous resources can be controlled from user space

**Success criterion:** a standalone user-space diagnostic program communicates with the FW410 without loading the original M-Audio kext.

### M2 — FireWire transport layer

- [ ] Node discovery
- [ ] Bus reset handling
- [ ] Addressing
- [ ] Asynchronous transactions
- [ ] Isochronous channels
- [ ] CIP handling
- [ ] Bandwidth/channel allocation

### M3 — FW410 / BeBoB protocol

- [ ] Device identification
- [ ] Firmware version
- [ ] Firmware loading / boot sequence
- [ ] AVC commands
- [ ] Clock
- [ ] Sample rates
- [ ] Stream configuration

### M4 — Audio DevKit

- [ ] Generic audio device abstraction
- [ ] Playback streams
- [ ] Capture streams
- [ ] Clock abstraction
- [ ] Sample-rate management
- [ ] Channel mapping
- [ ] Controls / mixer abstraction
- [ ] MIDI abstraction

### M5 — CoreAudio integration

- [ ] Minimal AudioDriverKit device
- [ ] Device enumeration
- [ ] Playback
- [ ] Capture
- [ ] Clock synchronization
- [ ] Controls

### M6 — FW410 complete implementation

- [ ] Mixer
- [ ] Routing
- [ ] Headphone output
- [ ] S/PDIF
- [ ] ADAT
- [ ] MIDI
- [ ] Firmware management
- [ ] Bus reset recovery

## External references

Existing open-source implementations will be used to validate the protocol model:

- Linux FireWire / BeBoB support
- Linux M-Audio FireWire support
- FFADO
- Other BeBoB implementations

These belong under [`reference/`](reference/) and are references, not assumptions that their architecture can be directly ported to macOS.

## Research status

**Current phase: M1 — User-space FireWire proof of concept**

The next implementation is a small diagnostic tool, not an audio driver. It must answer:

> **Can a normal user-space process on Intel macOS Sonoma communicate with the FW410 through the available FireWire interfaces, without the old M-Audio kext?**

Everything else depends on this result.

## Disclaimer

This repository is an independent reverse-engineering and compatibility project. It is not affiliated with, endorsed by, or supported by M-Audio, Avid, Apple, or any hardware manufacturer.

Vendor drivers, firmware, and other proprietary material remain subject to their respective licenses and copyrights.
