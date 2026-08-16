# FW410 current development status

Last updated: 2026-08-16

This is the canonical handoff document for continuing development in a fresh session. It records what has been confirmed on real M-Audio FireWire 410 hardware, what failed and why, the architecture decisions made so far, and the exact point at which development has reached CoreAudio/AudioDriverKit integration.

Treat Linux `snd-bebob`/FireWire and FFADO as protocol references, not implementations that must be copied. Prefer a clean macOS-native design while preserving behavior already proven on hardware.

## Executive status

The project has crossed the main reverse-engineering threshold needed to begin the real macOS audio-device integration.

Hardware-confirmed today:

- FW410 bootloader -> operational personality from user space;
- asynchronous FireWire access and AV/C/FCP control;
- IRM/CMP duplex connection management;
- NuDCL isochronous receive and transmit;
- AM824 capture decoding;
- 48 kHz PCM playback and capture;
- reusable live PCM ring-buffer playback;
- native 44.1 kHz PCM playback after discovering the M-Audio startup quirk;
- native 44.1 kHz CoreAudio -> FW410 bridge with no sample-rate conversion;
- temporary 44.1 -> 48 kHz bridge, useful as a fallback/reference but audibly inferior to native 44.1 on the tested path;
- sample-rate read/change at 44.1/48 kHz;
- mixer/topology probing and a verified headphone selector path.

The next primary milestone is no longer another packet-format experiment. It is to publish a real CoreAudio-facing `M-Audio FireWire 410` device through AudioDriverKit and then connect its audio buffers to the proven user-space FireWire engine.

## Test environment

Confirmed hardware testing has been performed on an Intel Mac with the M-Audio FireWire 410 using Apple's user-space `IOFireWireLib` path. The FW410 can be booted from its bootloader personality into the operational `FW 410` personality entirely from user space.

Confirmed identity transition:

```text
before: FW Bootloader / model 0x00010058
after:  FW 410        / model 0x00010046
```

## Repository organization

Responsibilities are intentionally separated:

- `fw410/tools/device/` — discovery, ROM, boot and device-level probes;
- `fw410/tools/control/` — AV/C, topology, plug, mixer, headphone and rate controls;
- `fw410/tools/transport/` — CMP, isochronous, AMDTP, PCM and CoreAudio bridge experiments;
- `fw410/lib/` — reusable transport/audio components extracted from successful experiments;
- `fw410/driverkit/` — CoreAudio/AudioDriverKit integration bootstrap and future real driver sources;
- `fw410/analysis/` — reverse-engineering results and handoff documentation.

The control/transport split must remain. A future control-panel application should be able to use the control layer without owning PCM/ISO implementation details.

## FireWire and device bring-up

Normal user-space code using `IOFireWireLib` can:

- find the bootloader and operational unit;
- read the configuration ROM;
- boot the FW410 into its operational personality;
- perform async reads/writes;
- send FCP/AV/C transactions;
- allocate ISO resources;
- establish and restore CMP/PCR state;
- run duplex isochronous programs.

A long-running production service must still implement robust bus-reset/generation recovery. Sample-rate changes can cause a FireWire generation/node change, so stale node/generation state must never be assumed valid indefinitely.

## Confirmed 48 kHz transport and PCM

48 kHz remains the reference transport path.

### Capture

FW410 -> Mac capture works with reusable NuDCL receive infrastructure.

Observed 48 kHz capture formation:

- FMT `0x10` (AM824);
- FDF `0x02`;
- DBS `5`;
- four PCM positions + one MIDI position;
- 8 events in each data-bearing packet;
- repeating 3 data / 1 NODATA blocking cadence.

Physical mapping was verified: front Analog Input 1 appears at PCM stream position 2.

### Playback

Mac -> FW410 playback works with real PCM. Playback formation is 10 PCM channels + one MIDI slot (`DBS=11`). Physical output mapping/tone tests were verified, including independent tones on Analog Outputs 1 and 2.

A critical implementation lesson: the known-good TX behavior came from manually constructed old-style NuDCL transmit programs. An early reusable abstraction produced zero data-bearing capture packets. A/B comparison found the regression and the abstraction was corrected while preserving the known-good NuDCL behavior. Future refactors must continue to be hardware-verified.

### Live PCM infrastructure

Reusable pieces implemented and tested include:

- `PcmBufferView`;
- `PcmRingBuffer`;
- `AmdtpTransmitRing`;
- `AmdtpPcmStream48k`;
- the corresponding 44.1 scheduling/live path developed from the native-rate work.

`pcmstreamplayback` demonstrated live ring-buffer playback with an independent producer thread and zero PCM underruns in a representative successful run. This validated the asynchronous producer -> FireWire scheduler architecture later used by the CoreAudio bridge.

## Native 44.1 kHz breakthrough

Native 44.1 kHz host playback is hardware-confirmed.

The packet scheduler was not the fundamental blocker. The decisive requirement is an M-Audio-specific stream-start sequence consistent with behavior seen in Linux `snd-bebob` references:

1. Switch both FW410 signal-format directions to 44100 Hz.
2. Establish both CMP connections.
3. Start duplex ISO streaming.
4. Initially transmit valid 44.1 kHz AMDTP NODATA.
5. While duplex streaming is live, reassert AV/C 44100 on both OUTPUT plug 0 and INPUT plug 0.
6. Continue into the normal native blocking 44.1 kHz data-bearing schedule.

The successful proof used 512 NODATA cycles (64 ms), with the AV/C 44100 reassertion approximately 20 ms after TX actually began. A 1 kHz tone was then audible on Analog Output 1.

Representative proof:

- OUTPUT 44100 reassert accepted;
- INPUT 44100 reassert accepted;
- 256 / 256 capture slots touched;
- about 176/177 data-bearing slots in a representative 256-slot observation;
- FDF `0x01` throughout the 44.1 capture observation;
- audible PCM output.

The diagnostic proof tool is `fw410/tools/transport/pcm44100warmup/`. The M-Audio reassertion is a **device/startup quirk**, not generic AMDTP packet-generation behavior.

## 44.1 kHz packet characterization

`transport/amdtp44probe` established the native 44.1 clock/cadence case.

Observed capture at 44.1 kHz:

- FDF `0x01`;
- DBS `5`;
- data packets remain 168 bytes / 8 events;
- representative 256-slot observations produce roughly 176/177 data-bearing packets and the remainder NODATA;
- DBC advances by 8 on data packets and is retained across NODATA;
- SYT follows the expected fractional base-44.1 sequence.

Playback remains 10 PCM + 1 MIDI (`DBS=11`). The blocking base-44.1 scheduler corresponding to the Linux initial state (`last_syt_offset = 3072`, base-44.1 SYT state 67) is accepted once the M-Audio post-start AV/C reassertion is performed.

## What the failed 44.1 experiments eliminated

Do not reopen these hypotheses without new hardware evidence:

1. AV/C rate selection was correct.
2. FDF `0x01` was correct.
3. DBS `11` / 10 PCM + 1 MIDI was correct.
4. CMP and NuDCL paths worked at 44.1.
5. TX start lead was sufficient.
6. The base-44.1 DBC/SYT/cadence implementation was fundamentally correct.
7. A simple NODATA warm-up alone was insufficient.
8. The missing action was the post-start AV/C rate reassertion while duplex AMDTP was live.

## CoreAudio bridge experiments

### AUHAL diagnostic

The tested Mac default input device is natively 44.1 kHz. `coreaudiodiag` isolated AUHAL capture and confirmed valid rendered frames with zero render errors before FireWire was introduced.

### Earlier 48 kHz FW410 bridge

Before native 44.1 playback was solved, CoreAudio input at 44.1 kHz was bridged to the FW410 running at 48 kHz. This proved the CoreAudio producer -> PCM ring -> FireWire transport architecture.

Several SRC variants were tested. AUHAL-side conversion initially produced render failures in one experiment (`-10863`) and no audio. A bridge-owned continuous SRC restored working audio. A cubic experiment sounded worse than the preceding linear implementation and was reverted. The final retained 48 kHz experimental path uses continuous linear 44.1 -> 48 kHz conversion.

This 48 kHz bridge is functional but the user reported that its audio is not as clear as the native 44.1 path. It should therefore remain a compatibility/reference path rather than the preferred architecture when both clock domains can run natively at 44.1 kHz.

### Native 44.1 CoreAudio bridge — preferred path

`coreaudiobridge44100` is hardware-confirmed working with clear audio and no bridge SRC.

Representative successful run:

```text
FW410 clock domain:     44100 Hz
CoreAudio native rate:  44100 Hz
AUHAL client:           44100 Hz mono Float32
bridge SRC:             none
PCM FIFO:               16384 frames
TX ring:                640 cycles / two 320-cycle halves
scheduler:              AmdtpPcmStream44100
startup quirk:          post-start AV/C 44100 reassert before first TX cycle
```

The run changed the FW410 from 48 kHz to 44.1 kHz, started duplex ISO, waited so the required reassert occurred about 20 ms after TX began, reasserted both AV/C directions, streamed actual CoreAudio input, and restored the FW410 to 48 kHz afterward.

Representative statistics:

```text
CoreAudio callbacks:    716
CoreAudio input frames: 366592
PCM written frames:     366592
PCM dropped frames:     0
CoreAudio errors:       0
PCM consumed frames:    361624
PCM underrun frames:    0
TX halves refilled:     203
late cycle polls:       35
```

This is currently the best-sounding and preferred proof path because there is no sample-rate conversion between the tested CoreAudio source and the FW410 clock domain.

## Sample-rate control

`control/rateprobe` reads and changes FW410 signal format in both directions. 48 -> 44.1 and 44.1 -> 48 transitions have been confirmed by AV/C readback on both device OUTPUT/host-capture and device INPUT/host-playback plugs.

For native 44.1 startup, the post-start 44100 reassert on both directions is mandatory on the tested hardware.

## Supported stream formations

`formatprobe` confirms:

| Rate | Host capture | Host playback | MIDI |
|---|---:|---:|---:|
| 44.1 kHz | 4 PCM | 10 PCM | 1 each direction |
| 48 kHz | 4 PCM | 10 PCM | 1 each direction |
| 88.2 kHz | 4 PCM | 10 PCM | 1 each direction |
| 96 kHz | 4 PCM | 10 PCM | 1 each direction |
| 176.4 kHz | 2 PCM | 8 PCM | 1 each direction |
| 192 kHz | 2 PCM | reduced/8-class formation per probe | 1 each direction |

Higher rates have been probed for formation but are not yet equivalent to the hardware-validated 44.1/48 live PCM paths.

## Mixer/routing/headphone findings

`topologyprobe` mapped the BridgeCo/AV/C Audio and Music subunit topology, feature blocks, processing blocks and selector blocks. `plugprobe` reports external output plugs including analog, digital and MIDI endpoints.

Seven selector function blocks (FB1..FB7) respond to STATUS and initially report input plug 0.

A guarded `headphoneprobe` changed selector FB7 from input 0 to input 1 temporarily and restored it afterward. Playback channel 1 was heard in headphone left and playback channel 2 in headphone right. This confirms internal FW410 headphone routing and identifies FB7 as relevant to that path.

Mixer/control-panel work remains intentionally deferred until basic CoreAudio device integration works.

## AudioDriverKit transition

The project has now started the real CoreAudio-facing driver phase under `fw410/driverkit/`.

The initial milestone is deliberately small: publish a synthetic `M-Audio FireWire 410` device in Audio MIDI Setup with a stereo output stream and 44.1/48 kHz advertised rates. The first stream may discard audio. Its purpose is to validate enumeration, AudioDriverKit lifecycle, host access and the real-time buffer contract before FireWire transport is connected.

Bootstrap files already added:

- `fw410/driverkit/README.md`;
- `fw410/driverkit/FW410AudioDriver.entitlements.template`;
- `fw410/driverkit/Info.plist.personality.template`.

The planned architecture is:

```text
CoreAudio applications
        |
CoreAudio HAL
        |
FW410 AudioDriverKit dext
        |
shared-memory / IPC audio boundary
        |
macfw transport service
        |
IOFireWireLib + AV/C/CMP/NuDCL/AMDTP
        |
M-Audio FireWire 410
```

The split is intentional. `IOFireWireLib` is already hardware-proven in an ordinary user-space process, while current DriverKit does not provide a FireWire transport family analogous to USBDriverKit/PCIDriverKit. Do not move the FireWire engine into the dext merely for architectural neatness unless a supported mechanism is demonstrated.

The dext should own the CoreAudio-facing device/stream abstraction. The transport service should own FireWire, device startup, rate control, CMP, the M-Audio startup quirk, ISO scheduling and bus-reset recovery. The boundary between them should be narrow: shared audio buffers/rings plus explicit control/state IPC.

## Architecture decisions to preserve

- Keep device/control/transport/CoreAudio responsibilities separated.
- Extract proven behavior into reusable `fw410/lib` components rather than leaving it only in probes.
- Preserve manually validated NuDCL behavior during abstraction/refactoring.
- Model the M-Audio post-start rate reassertion as a FW410 startup quirk.
- Keep the initial NODATA phase explicit in the stream-start state machine.
- Prefer native rates whenever both endpoints can share a rate; avoid unnecessary SRC.
- Preserve and restore exact PCR state around experiments/failures.
- Treat FireWire generation changes and bus resets as normal recoverable events in the production service.
- Keep the AudioDriverKit dext independent from BeBoB/CIP/NuDCL details.
- Keep the long-term goal reusable for other legacy FireWire audio devices rather than hard-wiring every layer to FW410.

## Release policy

Release/version/package policy is documented at repository root in `RELEASES.md`.

The agreed version format is `x.yy.zzz`:

- `x`: major generation;
- `yy`: larger update / feature addition;
- `zzz`: patch or minor fix.

Once there is a reproducibly buildable distributable driver, a tag-driven GitHub Actions release workflow will produce three packages from the same tag:

- `lite` — driver/runtime only;
- `full` — driver/runtime plus project tools;
- `source` — exact tagged repository snapshot as `.tar.gz`.

## Next work

1. Create the real Xcode macOS host application + Audio Driver Extension target under `fw410/driverkit/`.
2. Replace the generated AudioDriverKit sample identity with the FW410 driver/device classes.
3. Make `M-Audio FireWire 410` enumerate in Audio MIDI Setup with a stereo output stream.
4. Confirm ordinary CoreAudio applications can open/write that stream, initially even if output is discarded.
5. Define the dext <-> transport-service shared-memory/control protocol.
6. Connect the dext output stream first to the native 44.1 kHz proven transport path.
7. Add native 48 kHz device operation without involving 44.1 -> 48 SRC when the application/device are actually operating at 48 kHz.
8. Add capture and expose the complete physical channel set.
9. Add bus-reset/rate-change recovery suitable for a persistent service.
10. Return to mixer/routing/headphone, S/PDIF and MIDI integration.
11. Once the first distributable build is reproducible, implement the tag-driven `lite` / `full` / `source` GitHub release workflow described in `RELEASES.md`.

## Quick handoff summary

> The FW410 user-space FireWire stack is hardware-proven. 48 kHz capture/playback/live streaming works. Native 44.1 playback works after the required M-Audio startup ritual: duplex ISO with initial NODATA, then AV/C 44100 reasserted on both directions while streaming is live. A native 44.1 CoreAudio -> FW410 bridge is also confirmed, with no SRC, zero CoreAudio errors/drops/PCM underruns in a representative run, and subjectively clearer audio than the experimental 44.1 -> 48 kHz bridge. The project has therefore moved to AudioDriverKit integration. The immediate goal is a synthetic stereo `M-Audio FireWire 410` device visible to CoreAudio; after enumeration works, its buffers will be bridged to the existing user-space `IOFireWireLib` transport service. Release policy is already defined as tag-driven `x.yy.zzz` with lite/full/source packages once a reproducible distributable driver exists.
