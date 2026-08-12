# M-Audio FireWire 410 — Modern macOS Driver

Reverse-engineering and driver development project for the **M-Audio FireWire 410 (FW410)** audio interface, with the goal of providing support on modern Intel-based macOS systems, initially targeting **macOS Sonoma and newer**.

The original M-Audio driver is no longer maintained by the manufacturer.

## Project status

**Status: Reverse engineering / research**

No functional modern macOS driver exists yet.

The first stage of this project is to understand the original M-Audio driver, the FW410 hardware protocol, its firmware requirements, and the FireWire audio transport.

---

## Hardware

**Device:** M-Audio FireWire 410

**Interface:** IEEE 1394 / FireWire

**Device family:** BeBoB-based FireWire audio device

The FW410 provides:

- FireWire audio streaming
- Multiple analog inputs and outputs
- S/PDIF
- MIDI
- Headphone output
- Hardware mixer/routing
- Multiple sample rates
- Clock/source configuration

Exact capabilities and hardware/firmware revisions will be documented under [`docs/hardware/`](docs/hardware/).

---

## Goal

The primary goal is to make the M-Audio FireWire 410 usable as a normal CoreAudio device on modern Intel Macs.

The initial target is:

- macOS Sonoma
- Intel Macs
- FireWire 410

Newer macOS versions will be investigated after the Sonoma target is understood and working.

Apple Silicon / ARM support is **not currently a project target**.

---

## Approach

This project will **not initially attempt to port the original M-Audio kernel extension directly**.

Instead, the original driver will be treated as a reference implementation.

The project will proceed in the following stages:

```text
                    M-Audio FireWire 410
                              │
                              ▼
                  ┌─────────────────────┐
                  │ Hardware / Protocol │
                  └──────────┬──────────┘
                             │
                             ▼
                  ┌─────────────────────┐
                  │ Reverse Engineering │
                  │                     │
                  │ Original M-Audio    │
                  │ driver              │
                  └──────────┬──────────┘
                             │
              ┌──────────────┴──────────────┐
              │                             │
              ▼                             ▼
       Linux / BeBoB                   FFADO
       implementation                 implementation
              │                             │
              └──────────────┬──────────────┘
                             │
                             ▼
                  ┌─────────────────────┐
                  │ Protocol Definition│
                  └──────────┬──────────┘
                             │
                             ▼
                  ┌─────────────────────┐
                  │ Modern macOS        │
                  │ implementation      │
                  └──────────┬──────────┘
                             │
                             ▼
                        CoreAudio
```

---

# 1. Original driver analysis

The original M-Audio driver is preserved under:

```text
original/
```

The currently available driver is:

```text
M-AudioFireWireBeBoB.kext
```

The original kext must be treated as immutable evidence.

Initial observations:

- The supplied binary is **x86_64**
- It is therefore not simply a 32-bit driver requiring conversion
- It is a legacy kernel extension
- It depends on Apple's legacy FireWire and audio kernel frameworks
- It contains M-Audio-specific FireWire/BeBoB implementation code
- The binary contains useful class names, source filenames, diagnostic strings and firmware-related symbols

Relevant components discovered so far include:

```text
FWIsochChannel
FWDCLProgram
FWDCLInputProgram
FWDCLInputProgram
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

These will be mapped during the reverse-engineering phase.

---

# 2. Reverse-engineering objectives

The original driver will be analyzed to determine:

### Device discovery

- FireWire device matching
- Vendor/device identifiers
- Configuration ROM handling
- Device initialization
- Bus reset handling

### Firmware

- Firmware version detection
- Bootloader interaction
- Firmware upload
- Firmware activation
- Firmware configuration
- Firmware persistence

### FireWire transport

- Asynchronous transactions
- Isochronous channels
- CIP packets
- DCL programs
- Connection management
- Bus reset/recovery

### Audio

- Playback streams
- Capture streams
- Channel configuration
- Sample rates
- Clock source
- Synchronization
- Stream start/stop

### Mixer and controls

- Input routing
- Output routing
- Mixer levels
- Headphone routing
- S/PDIF
- ADAT
- Hardware controls

### MIDI

- MIDI transport
- Device initialization
- MIDI input/output

---

# 3. External reference implementations

Existing open-source implementations will be used as protocol references.

Important references include:

- Linux FireWire BeBoB support
- Linux M-Audio FireWire support
- FFADO
- Other BeBoB-based devices

These implementations should be kept under:

```text
reference/
```

and documented rather than blindly copied into the new implementation.

The objective is to determine which behavior is:

1. Generic IEEE 1394 behavior
2. Generic BeBoB behavior
3. M-Audio-specific behavior
4. FW410-specific behavior

---

# 4. Modern macOS architecture

The final implementation architecture has not yet been decided.

Possible approaches include:

### Option A — AudioDriverKit

Use Apple's modern audio-driver architecture.

```text
CoreAudio
    │
    ▼
AudioDriverKit
    │
    ▼
FW410 implementation
    │
    ▼
FireWire transport
```

### Option B — User-space FireWire transport

If a suitable FireWire transport mechanism can be made available on modern macOS, keep as much of the implementation as possible in user space.

### Option C — Compatibility / transport layer

If modern macOS does not expose sufficient FireWire functionality, investigate a dedicated compatibility layer.

### Option D — Kernel component

Only consider a kernel component if the previous approaches cannot provide the required FireWire functionality.

The project should avoid committing to a kernel-extension architecture until the FireWire transport problem is understood.

---

# 5. Repository organization

```text
docs/
```

Design documents, reverse-engineering notes and hardware documentation.

```text
original/
```

Original vendor material. This directory should remain immutable.

```text
analysis/
```

Static-analysis artifacts produced from the original driver.

```text
protocol/
```

Our reconstructed understanding of the FW410 and BeBoB protocols.

```text
captures/
```

FireWire traffic captures and other hardware observations.

```text
reference/
```

External implementations such as Linux and FFADO.

```text
experiments/
```

Temporary prototypes and experimental code.

```text
driver/
```

The eventual modern macOS driver implementation.

```text
tools/
```

Scripts and utilities developed during reverse engineering.

```text
tests/
```

Automated and hardware-based tests.

---

# 6. Development principles

### Preserve evidence

Never modify the original vendor files.

### Document discoveries

Every significant reverse-engineering discovery should be documented.

### Separate facts from assumptions

When documenting protocol behavior, distinguish between:

- Confirmed
- Observed
- Inferred
- Unknown

### Prefer protocol compatibility over code compatibility

The objective is not to reproduce the original M-Audio source code.

The objective is to reproduce the behavior required by the FW410 hardware.

### Minimize kernel code

Modern macOS compatibility should be achieved with the smallest possible privileged component.

### Test incrementally

The implementation should progress through small milestones:

```text
Device detection
      ↓
FireWire communication
      ↓
Firmware identification
      ↓
Device initialization
      ↓
Clock/sample-rate control
      ↓
Playback
      ↓
Capture
      ↓
Multiple channels
      ↓
Mixer
      ↓
MIDI
      ↓
Complete device
```

---

# 7. Initial milestones

## M1 — Repository and evidence

- [x] Preserve original M-Audio kext
- [ ] Calculate hashes
- [ ] Extract kext
- [ ] Document Info.plist
- [ ] Identify Mach-O architecture
- [ ] Extract strings
- [ ] Extract symbols
- [ ] Import into Ghidra
- [ ] Create initial class map

## M2 — Hardware identification

- [ ] Identify FW410 hardware revision
- [ ] Identify firmware revision
- [ ] Capture configuration ROM
- [ ] Document FireWire identifiers
- [ ] Document available interfaces/endpoints

## M3 — Firmware

- [ ] Identify firmware format
- [ ] Identify firmware loading mechanism
- [ ] Identify firmware version query
- [ ] Document bootloader protocol
- [ ] Determine whether firmware is required on every boot

## M4 — FireWire protocol

- [ ] Map asynchronous transactions
- [ ] Map AVC commands
- [ ] Map isochronous streams
- [ ] Map CIP format
- [ ] Map connection setup
- [ ] Map bus reset handling

## M5 — Audio protocol

- [ ] Playback
- [ ] Capture
- [ ] Sample rates
- [ ] Clock source
- [ ] Channel configuration

## M6 — Modern macOS prototype

- [ ] Determine Sonoma FireWire access strategy
- [ ] Create minimal CoreAudio device
- [ ] Establish device communication
- [ ] Start playback
- [ ] Start capture

## M7 — Full device

- [ ] Mixer
- [ ] Routing
- [ ] Headphone output
- [ ] S/PDIF
- [ ] ADAT
- [ ] MIDI
- [ ] Firmware management
- [ ] Recovery from FireWire bus reset

---

# 8. Current priority

The immediate priority is **not driver development**.

The immediate priority is:

```text
Original kext
     │
     ▼
Static analysis
     │
     ▼
FW410 class/function map
     │
     ▼
Protocol reconstruction
     │
     ├─────────────┐
     ▼             ▼
Linux/FFADO     Hardware captures
     │             │
     └──────┬──────┘
            ▼
    Protocol specification
            │
            ▼
    Sonoma architecture
```

Once the FireWire transport requirements are understood, the modern macOS implementation can be selected based on evidence rather than assumptions.

---

## Disclaimer

This repository is an independent reverse-engineering and compatibility project.

M-Audio/Avid is not currently providing support for this project or for modern macOS compatibility of the FireWire 410.

The original vendor driver is retained for research and compatibility analysis. Redistribution of proprietary vendor material should be handled according to applicable licensing and copyright requirements.