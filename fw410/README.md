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

## Confirmed user-space result

On an Intel Mac running macOS Monterey, a normal user-space process using Apple's `IOFireWireLib` can:

- discover the FW410 bootloader;
- read its configuration ROM;
- perform direct asynchronous FireWire reads;
- read and decode the BeBoB information registers;
- issue the guarded M-Audio boot-from-flash cue;
- survive the resulting FireWire bus reset by reacquiring generation/node state;
- rediscover the device as the operational `FW 410` personality.

Confirmed identity transition:

```text
before: FW Bootloader / model 0x00010058 / generation 144
after:  FW 410        / model 0x00010046 / generation 145
```

The operational BeBoB information block reports bootloader version `0`, confirming that the application firmware is running.

## Protocol layers

```text
┌───────────────────────────────────────┐
│ CoreAudio / AudioDriverKit            │
├───────────────────────────────────────┤
│ macfw Audio DevKit                    │
│ streams / clock / controls / MIDI     │
├───────────────────────────────────────┤
│ Device protocol                       │
│ FW410 / BeBoB / AV/C                  │
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

The original M-Audio driver is treated as immutable reference material. Existing Linux `snd-bebob` and FFADO/FreeBoB implementations are used as independent protocol references.

Important classes discovered in the original driver include:

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

See [`analysis/`](analysis/) for reverse-engineering notes.

## Development milestones

### M0 — Repository and evidence

- [x] Preserve original driver information
- [x] Identify Mach-O architecture
- [ ] Record hashes of original files
- [ ] Extract and document `Info.plist`
- [ ] Complete initial class/function map

### M1 — User-space FireWire proof of concept

- [x] Detect an attached FireWire controller/device from user space
- [x] Detect the FW410 bootloader
- [x] Read the configuration ROM
- [x] Read bus generation and remote node ID
- [x] Perform harmless asynchronous reads
- [x] Perform a narrowly scoped asynchronous write
- [x] Start the FW410 operational firmware from flash
- [x] Rediscover the operational FW410 after bus reset
- [ ] Determine whether isochronous resources can be controlled from user space
- [ ] Repeat the successful path on Intel macOS Sonoma or newer

**M1 core success criterion achieved:** a standalone user-space program communicates with and boots the FW410 without loading the original M-Audio kext.

### M2 — FireWire transport layer

- [x] Node discovery
- [ ] Bus reset notification/recovery abstraction
- [x] 64-bit asynchronous addressing
- [x] Asynchronous block read/write primitives
- [ ] FCP command/response transport
- [ ] Isochronous channels
- [ ] CIP handling
- [ ] Bandwidth/channel allocation

### M3 — FW410 / BeBoB protocol

- [x] Device identification
- [x] Firmware information
- [x] Firmware boot-from-flash sequence
- [ ] AV/C commands
- [ ] Plug discovery
- [ ] Clock source discovery/control
- [ ] Sample-rate discovery/control
- [ ] Stream configuration
- [ ] MIDI/control protocol mapping

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
- [ ] MIDI
- [ ] Firmware management
- [ ] Bus reset recovery

## Current phase

**M2/M3 boundary — operational-device protocol discovery.**

The next task is to implement a minimal AV/C Function Control Protocol transport in user space and use read-only AV/C STATUS commands to discover the operational FW410's plug configuration and current sample rate before any audio streaming is attempted.

## External references

Existing open-source implementations are protocol references:

- Linux FireWire / `snd-bebob`
- Linux M-Audio FireWire support
- FFADO / FreeBoB
- IEEE 1394 / IEC 61883 / AV/C specifications where available

## Disclaimer

This repository is an independent reverse-engineering and compatibility project. It is not affiliated with, endorsed by, or supported by M-Audio, Avid, Apple, or any hardware manufacturer.

Vendor drivers, firmware, and other proprietary material remain subject to their respective licenses and copyrights.
