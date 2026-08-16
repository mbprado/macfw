# M-Audio FireWire 410 — Modern macOS Driver / DevKit

Reverse-engineering and **user-space driver / DevKit** project for the M-Audio FireWire 410 (FW410).

## Primary goal

The goal is **not** to port the original M-Audio kernel extension.

The goal is to develop a modern macOS FireWire audio driver/development kit that can eventually be reused by other legacy FireWire audio interfaces. The FW410 is the first hardware backend used to develop and validate the stack.

```text
CoreAudio / AudioDriverKit
          |
   macfw Audio DevKit
          |
   FW410 / BeBoB backend
          |
FireWire user-space transport
          |
        FW410
```

The current architecture keeps the AudioDriverKit/CoreAudio-facing component separate from the already proven `IOFireWireLib` transport service.

## Non-goals

- Porting the original kext line-by-line.
- Maintaining a new legacy kernel extension as the primary architecture.
- Reproducing M-Audio's original source architecture.
- Apple Silicon support at this stage.

A kernel component should only be considered if a required low-level capability cannot be implemented through the modern/user-space stack.

## Hardware

**Device:** M-Audio FireWire 410  
**Interface:** IEEE 1394 / FireWire  
**Device family:** BeBoB-based FireWire audio device

## Confirmed results

On the tested Intel Mac, normal user-space processes using Apple's `IOFireWireLib` can:

- discover and boot the FW410 from its bootloader personality;
- perform asynchronous FireWire reads/writes and raw FCP/AV/C transactions;
- discover plug topology, supported rates, stream formations and channel positions;
- allocate IRM channel/bandwidth resources and establish both CMP directions;
- transmit and receive NuDCL isochronous programs entirely from user space;
- decode FW410 AM824 capture to signed 24-bit PCM;
- transmit correctly timed 48 kHz data-bearing AM824 playback;
- play verified tones on physical FW410 analog outputs;
- feed playback from reusable PCM/ring buffers with live refill;
- run an independent PCM producer against the FireWire scheduler;
- run 48 kHz and native 44.1 kHz PCM transport;
- capture CoreAudio/AUHAL input and reproduce it through FW410 outputs;
- run the CoreAudio -> FW410 bridge natively at 44.1 kHz with **no SRC**;
- read and change the FW410 sample rate through AV/C;
- probe internal AV/C mixer/routing topology and identify a headphone selector path.

Confirmed boot identity transition:

```text
before: FW Bootloader / model 0x00010058
after:  FW 410        / model 0x00010046
```

## Native 44.1 kHz result

Native 44.1 kHz playback is solved and hardware-confirmed.

The FW410 requires an M-Audio-specific startup ritual:

1. select 44100 in both AV/C signal-format directions;
2. establish duplex CMP/ISO;
3. begin with valid 44.1 AMDTP NODATA;
4. while duplex streaming is live, reassert 44100 on both OUTPUT plug 0 and INPUT plug 0;
5. continue into the normal blocking 44.1 data-bearing schedule.

With that sequence the existing `DBS=11`, `FDF=0x01`, base-44.1 scheduler produces audible PCM and the FW410 returns to normal sample-bearing capture.

The native `coreaudiobridge44100` path is also hardware-confirmed. On the tested Mac both CoreAudio and FW410 operate at 44100 Hz, so the bridge requires no sample-rate conversion. It is subjectively clearer than the experimental 44.1 -> 48 kHz bridge.

For exact observations, statistics, failed experiments already eliminated and the current handoff, read [`analysis/current-development-status.md`](analysis/current-development-status.md).

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

## Development milestones

### M1 — User-space FireWire proof of concept

- [x] Detect/boot the FW410 from user space
- [x] Read configuration ROM and BeBoB information
- [x] Async read/write
- [x] FCP/AV/C command/response
- [x] Allocate isochronous resources
- [x] Establish duplex transport
- [ ] Repeat successful path on Intel macOS Sonoma or newer

### M2 — FireWire transport layer

- [x] Node discovery
- [ ] Bus-reset notification/recovery abstraction
- [x] Async block read/write primitives
- [x] FCP transport
- [x] IRM allocation and CMP connection setup/teardown
- [x] NuDCL RX and TX
- [x] CIP/AM824 parsing
- [x] Reusable RX/TX ring infrastructure
- [x] 48 kHz live PCM refill
- [x] Native 44.1 kHz live PCM path
- [x] Independent producer + transport scheduler
- [ ] Long-running production stream engine
- [ ] Bus-reset-safe stream restart

### M3 — FW410 / BeBoB protocol

- [x] Device/firmware identification and boot
- [x] Plug and stream-formation discovery
- [x] PCM/MIDI channel mapping
- [x] Duplex packet-flow requirement
- [x] Sample-rate discovery
- [x] Sample-rate control (44.1/48 tested)
- [x] M-Audio native-44.1 startup quirk identified
- [x] Mixer/selector topology discovery
- [x] Headphone selector candidate verified experimentally
- [ ] Clock source discovery/control
- [ ] Complete mixer/control mapping
- [ ] MIDI byte transport validation

### M4 — Audio DevKit

- [ ] Generic audio-device abstraction
- [x] Reusable FireWire TX/RX components
- [x] Playback PCM encoder
- [x] Capture PCM decoder
- [x] PCM buffer/ring-buffer abstraction
- [x] 48 kHz reusable PCM stream scheduler
- [x] Native 44.1 scheduler/playback path
- [ ] Unified sample-rate management
- [ ] Controls/mixer abstraction
- [ ] MIDI abstraction

### M5 — CoreAudio integration

- [x] AUHAL input diagnostic
- [x] Experimental 44.1 -> 48 kHz CoreAudio bridge
- [x] Native 44.1 kHz CoreAudio -> FW410 bridge with no SRC
- [x] AudioDriverKit bootstrap/design started
- [ ] CoreAudio-facing FW410 device enumerates in Audio MIDI Setup
- [ ] Playback exposed to applications
- [ ] DriverKit <-> FireWire transport-service buffer bridge
- [ ] Capture exposed to applications
- [ ] Clock/rate synchronization
- [ ] Controls

### M6 — FW410 complete implementation

- [ ] Mixer/control panel
- [ ] Routing
- [ ] Headphone controls
- [ ] S/PDIF validation
- [ ] MIDI
- [ ] Firmware management
- [ ] Bus reset recovery

## Current architecture

The current intended production split is:

```text
CoreAudio application
        |
CoreAudio HAL
        |
AudioDriverKit dext
        |
shared-memory / IPC boundary
        |
macfw transport service
        |
IOFireWireLib
        |
FW410
```

`IOFireWireLib` is already proven on hardware in ordinary user space. The AudioDriverKit dext should own the macOS audio-device abstraction, while the companion service owns FireWire/device protocol and ISO scheduling. This keeps unsupported/legacy transport details out of the dext and gives the FireWire engine a normal user-space environment.

The first AudioDriverKit milestone is intentionally synthetic: make `M-Audio FireWire 410` appear in Audio MIDI Setup with stereo output at 44.1/48 kHz, initially even if samples are discarded. Once that is stable, connect its output buffer to the proven transport engine.

See [`driverkit/README.md`](driverkit/README.md) for the bootstrap plan.

## Release policy

The project release contract is documented in [`../RELEASES.md`](../RELEASES.md).

Version format:

```text
x.yy.zzz
```

- `x` — major version;
- `yy` — bigger update / feature addition;
- `zzz` — patch or minor fix.

Once a distributable build is reproducible, tag-driven GitHub Actions releases will generate:

- `lite` — driver/runtime only;
- `full` — driver/runtime plus the project tools;
- `source` — exact tagged repository source as `.tar.gz`.

## Current phase

**AudioDriverKit/CoreAudio device integration.**

The transport proof phase has reached the point where both 48 kHz and native 44.1 kHz PCM are hardware-confirmed, and native 44.1 CoreAudio bridging is working cleanly without SRC. Development is therefore moving to the actual CoreAudio-facing FW410 device while preserving the existing FireWire engine as a user-space service.

Mixer/routing/headphone work remains deferred until basic driver playback/capture integration works.

## External references

Existing open-source implementations are protocol references:

- Linux FireWire / `snd-bebob`;
- Linux M-Audio FireWire support;
- FFADO / FreeBoB;
- IEEE 1394 / IEC 61883 / AV/C specifications where available.

Use these to reduce duplicated reverse engineering, but do not copy implementation choices blindly when macOS offers a cleaner approach.

## Disclaimer

This repository is an independent reverse-engineering and compatibility project. It is not affiliated with, endorsed by, or supported by M-Audio, Avid, Apple, or any hardware manufacturer.

Vendor drivers, firmware and other proprietary material remain subject to their respective licenses and copyrights.
