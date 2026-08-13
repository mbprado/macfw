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

On an Intel Mac running macOS Monterey, normal user-space processes using Apple's `IOFireWireLib` can now perform the complete low-level bring-up path:

- discover the FW410 bootloader;
- read its configuration ROM;
- perform direct asynchronous FireWire reads and writes;
- read and decode the BeBoB information registers;
- issue the guarded M-Audio boot-from-flash cue;
- survive the resulting FireWire bus reset by reacquiring generation/node state;
- rediscover the device as the operational `FW 410` personality;
- send AV/C commands through raw FCP from user space;
- receive FCP responses through a user-space pseudo address space;
- discover plug topology, sample rates, PCM/MIDI formations, and channel positions;
- allocate FireWire IRM channel/bandwidth resources;
- establish and tear down both CMP stream connections;
- create local NuDCL isochronous transmit and receive programs;
- transmit Mac -> FW410 isochronous packets;
- receive FW410 -> Mac AM824 audio packets;
- trigger the FW410's required full-duplex stream behavior entirely from user space.

Confirmed boot identity transition:

```text
before: FW Bootloader / model 0x00010058
after:  FW 410        / model 0x00010046
```

The operational BeBoB information block reports bootloader version `0`, confirming that the application firmware is running.

Confirmed AV/C status transaction:

```text
command:  01 ff 18 00 90 ff ff ff
response: 0c ff 18 00 90 02 ff ff
rate:     48000 Hz
```

Confirmed live 48 kHz capture packet:

```text
00 05 00 f0 90 02 aa b5 ...
```

This decodes as AM824, DBS=5, eight events per 168-byte data packet, valid SYT, and the expected four PCM + one MIDI position layout. With host playback ISO packets active, the FW410 produces a repeating 3-data / 1-NODATA blocking-mode cadence, exactly yielding 48,000 events/sec.

See [`analysis/isochronous-transport.md`](analysis/isochronous-transport.md) for packet-level evidence.

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
- [x] Determine whether isochronous resources can be controlled from user space
- [ ] Repeat the successful path on Intel macOS Sonoma or newer

**M1 achieved:** a standalone user-space process communicates with, boots, allocates ISO resources for, and streams packets with the FW410 without loading the original M-Audio kext.

### M2 — FireWire transport layer

- [x] Node discovery
- [ ] Bus reset notification/recovery abstraction
- [x] 64-bit asynchronous addressing
- [x] Asynchronous block read/write primitives
- [x] FCP command/response transport
- [x] Isochronous channel creation
- [x] IRM bandwidth/channel allocation
- [x] CMP connection setup/teardown
- [x] NuDCL receive path
- [x] NuDCL transmit path
- [x] CIP/AM824 header parsing
- [x] 48 kHz blocking-mode capture cadence identified
- [ ] Long-running ring-buffered ISO engine
- [ ] Bus-reset-safe stream restart

### M3 — FW410 / BeBoB protocol

- [x] Device identification
- [x] Firmware information
- [x] Firmware boot-from-flash sequence
- [x] AV/C STATUS command path
- [x] Plug discovery
- [ ] Clock source discovery/control
- [x] Sample-rate discovery in both directions
- [ ] Sample-rate control
- [x] Stream formation discovery
- [x] PCM/MIDI channel-position mapping
- [x] Confirm FW410 requires actual duplex packet flow
- [ ] MIDI byte transport validation
- [ ] Mixer/control protocol mapping

### M4 — Audio DevKit

- [ ] Generic audio device abstraction
- [ ] Reusable FireWire stream engine
- [ ] Playback PCM encoder
- [ ] Capture PCM decoder
- [ ] Clock/SYT abstraction
- [ ] Sample-rate management
- [x] FW410 channel mapping understood
- [ ] Controls / mixer abstraction
- [ ] MIDI abstraction

### M5 — CoreAudio integration

- [ ] Minimal AudioDriverKit/CoreAudio-facing device
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

**M2 transport completion / M4 Audio DevKit extraction.**

Raw FireWire audio transport has now been demonstrated in both directions. The FW410 starts real capture packets when the host sends an active isochronous playback stream, even when that playback stream contains AM824 NODATA only.

Immediate next work:

1. decode capture MBLA words into signed 24-bit PCM and gather per-channel statistics;
2. generate correctly timed data-bearing playback silence with continuous DBC/SYT;
3. validate longer-running duplex continuity;
4. refactor the experimental code into reusable transport/AM824 classes;
5. then expose the validated streams to the macOS audio layer.

## External references

Existing open-source implementations are protocol references:

- Linux FireWire / `snd-bebob`
- Linux M-Audio FireWire support
- FFADO / FreeBoB
- IEEE 1394 / IEC 61883 / AV/C specifications where available

## Disclaimer

This repository is an independent reverse-engineering and compatibility project. It is not affiliated with, endorsed by, or supported by M-Audio, Avid, Apple, or any hardware manufacturer.

Vendor drivers, firmware, and other proprietary material remain subject to their respective licenses and copyrights.