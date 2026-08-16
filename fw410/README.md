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

On an Intel Mac running macOS Monterey, normal user-space processes using Apple's `IOFireWireLib` can now perform the complete low-level bring-up path and substantially more of the audio stack:

- discover and boot the FW410 from its bootloader personality;
- perform asynchronous FireWire reads/writes and raw FCP/AV/C transactions;
- discover plug topology, supported rates, stream formations, and channel positions;
- allocate IRM channel/bandwidth resources and establish both CMP directions;
- transmit and receive NuDCL isochronous programs entirely from user space;
- decode FW410 AM824 capture to signed 24-bit PCM;
- transmit correctly timed 48 kHz data-bearing AM824 playback;
- play verified tones on physical FW410 analog outputs;
- feed playback from reusable PCM buffers/ring buffers with live refill;
- run an independent PCM producer thread against the FireWire scheduler;
- capture audio through CoreAudio/AUHAL and reproduce it through FW410 outputs;
- read and change the FW410 sample rate through AV/C;
- run valid 44.1 kHz NODATA duplex transport and receive sample-bearing 44.1 kHz capture;
- probe the internal AV/C mixer/routing topology and identify a headphone selector path.

Confirmed boot identity transition:

```text
before: FW Bootloader / model 0x00010058
after:  FW 410        / model 0x00010046
```

48 kHz PCM playback/capture and live streaming are now the known-good reference implementation. The current transport blocker is native **data-bearing 44.1 kHz host playback**, not basic 44.1 kHz rate selection or ISO connectivity.

For the detailed handoff and exact experiments already eliminated, read [`analysis/current-development-status.md`](analysis/current-development-status.md) before continuing development.

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
- [ ] General native-rate clock/SYT scheduler
- [ ] Unified sample-rate management
- [ ] Controls/mixer abstraction
- [ ] MIDI abstraction

### M5 — CoreAudio integration

- [x] AUHAL input diagnostic
- [x] Experimental CoreAudio input -> FW410 PCM bridge
- [x] Temporary 44.1 -> 48 kHz SRC bridge validated
- [ ] CoreAudio-facing FW410 device
- [ ] Playback exposed to applications
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

## Current phase

**Native-rate AMDTP completion, then CoreAudio integration.**

The 48 kHz path is hardware-confirmed end-to-end: capture, PCM playback, reusable buffers, live asynchronous refill, and an experimental CoreAudio-input bridge all work.

The FW410 also successfully changes to 44.1 kHz and produces sample-bearing 44.1 kHz capture when the host sends the known-good NODATA stream. `formatprobe` confirms that 44.1 kHz playback still uses 10 PCM + 1 MIDI (DBS=11). Native data-bearing 44.1 kHz playback, however, currently produces no sound and leaves FW410 capture in NODATA.

Known causes already eliminated include wrong rate selection, wrong FDF, wrong DBS/formation, general 44.1 CMP/NuDCL failure, and missing the scheduled TX start cycle. See [`analysis/current-development-status.md`](analysis/current-development-status.md) for the exact evidence and recommended A/B experiment.

After native-rate transport is resolved, development should return to the CoreAudio-facing device architecture. Mixer/routing/headphone work is intentionally deferred; selector FB7 has already been shown to route playback channels 1/2 to headphone left/right when switched to its alternate input.

## External references

Existing open-source implementations are protocol references:

- Linux FireWire / `snd-bebob`
- Linux M-Audio FireWire support
- FFADO / FreeBoB
- IEEE 1394 / IEC 61883 / AV/C specifications where available

Use these to reduce duplicated reverse engineering, but do not copy implementation choices blindly when macOS offers a cleaner or more efficient approach.

## Disclaimer

This repository is an independent reverse-engineering and compatibility project. It is not affiliated with, endorsed by, or supported by M-Audio, Avid, Apple, or any hardware manufacturer.

Vendor drivers, firmware, and other proprietary material remain subject to their respective licenses and copyrights.