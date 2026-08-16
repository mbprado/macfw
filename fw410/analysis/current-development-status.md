# FW410 current development status

Last updated: 2026-08-16

This is the handoff document for continuing development in a fresh session. It records what is confirmed on real FW410 hardware, the current architecture, and the exact unresolved problem. Treat the Linux `snd-bebob`/FireWire implementation and FFADO as protocol references, not code that must be copied blindly: prefer a cleaner/macOS-native implementation when appropriate.

## Test environment

Confirmed hardware testing has been performed on an Intel Mac with the M-Audio FireWire 410 using Apple's user-space `IOFireWireLib` path. The FW410 can be booted from its bootloader personality into the operational `FW 410` personality entirely from user space.

## Repository/tool organization

Tools are intentionally separated by responsibility:

- `fw410/tools/device/` — device/FireWire discovery and boot-related tools.
- `fw410/tools/control/` — AV/C, topology, plug, mixer, headphone, and sample-rate controls. These are not stream tools.
- `fw410/tools/transport/` — CMP, isochronous, AMDTP, PCM streaming, and CoreAudio bridge experiments.
- `fw410/lib/` — reusable transport/audio components extracted from successful experiments.

The control/transport split is deliberate. A future FW410 control-panel application can be built on the control work without mixing it into the PCM/ISO engine.

## Confirmed 48 kHz transport and PCM

48 kHz is the known-good reference path.

### Capture

FW410 -> Mac capture works with reusable NuDCL receive infrastructure. Capture format at 48 kHz is:

- FMT `0x10` (AM824)
- FDF `0x02`
- DBS `5`
- four PCM positions + one MIDI position
- 8 events in each data-bearing packet
- repeating 3 data / 1 NODATA blocking cadence

Physical mapping has been verified: front Analog Input 1 appears at PCM stream position 2.

### Playback

Mac -> FW410 playback is confirmed working with real PCM audio. The playback formation is 10 PCM channels + one MIDI slot (DBS=11). Physical output mapping/tone tests work, including independently generated lower/higher tones on Analog Outputs 1 and 2.

An important implementation discovery was that the working TX implementation uses manually constructed old-style NuDCL transmit programs. An attempted reusable abstraction initially produced zero data-bearing capture packets; A/B isolation against the old implementation found the regression and the reusable implementation was corrected while preserving the behavior of the known-good manual TX path.

### PCM buffer and live streaming

Reusable PCM infrastructure is implemented and tested:

- `PcmBufferView`
- `PcmRingBuffer`
- reusable `AmdtpTransmitRing`
- reusable `AmdtpPcmStream48k`

`pcmstreamplayback` successfully performs live ring-buffer playback with an independent producer thread. A representative successful run had zero PCM underrun frames, ~191k consumed frames over four seconds, and only one late cycle poll. Payload data can be updated in mmap memory while NuDCL metadata remains unchanged.

This validates the architecture needed for a CoreAudio producer feeding the FireWire transport asynchronously.

## CoreAudio bridge status

A CoreAudio input -> FW410 playback bridge has been demonstrated successfully.

The Mac's tested default input device runs natively at 44.1 kHz. AUHAL capture was first isolated with `coreaudiodiag`; it produced valid rendered frames with zero render errors. The bridge then captured the default CoreAudio input, converted mono Float32 samples into the FW410 PCM ring, and played them on FW410 Analog Outputs 1 and 2.

Because the FW410 transport was still running at the known-good 48 kHz rate, the bridge used a simple linear 44.1 -> 48 kHz sample-rate converter. This resampling is a temporary clock-domain bridge, not intended as a requirement of the final driver. Once native FW410 44.1 kHz transport is working, a 44.1 kHz CoreAudio stream should run natively without that SRC when both sides use the same rate.

Confirmed successful bridge characteristics included:

- CoreAudio input native rate: 44100 Hz
- AUHAL client: 44100 Hz mono Float32
- FW410 transport: 48000 Hz
- bridge SRC: 44100 -> 48000 linear
- actual microphone audio heard through FW410 outputs 1 and 2
- zero CoreAudio render errors
- zero PCM dropped frames in the successful test

CoreAudio integration is the next major project direction after native-rate transport is sufficiently understood.

## Sample-rate control

`control/rateprobe` can read and change the FW410 signal format in both directions. A 48 kHz -> 44.1 kHz transition has been confirmed by AV/C readback on both device OUTPUT/host-capture and device INPUT/host-playback plugs.

The earlier restore result incorrectly printed FAIL despite readback being 48000/48000; that reporting bug was corrected.

Changing sample rate can trigger a FireWire generation/node change, so tools must not assume that the original generation/node remains valid indefinitely. Reacquisition after reset remains an architectural requirement for the eventual long-running driver.

## Supported FW410 stream formations

`formatprobe` confirms:

| Rate | Host capture | Host playback | MIDI |
|---|---:|---:|---:|
| 44.1 kHz | 4 PCM | 10 PCM | 1 each direction |
| 48 kHz | 4 PCM | 10 PCM | 1 each direction |
| 88.2 kHz | 4 PCM | 10 PCM | 1 each direction |
| 96 kHz | 4 PCM | 10 PCM | 1 each direction |
| 176.4 kHz | 2 PCM | 8 PCM | 1 each direction |
| 192 kHz | 2 PCM | reduced/8-class formation per probe | 1 each direction |

For the current 44.1 kHz playback investigation, the key fact is that playback remains **10 PCM + 1 MIDI**, therefore **DBS=11 is correct**. Channel count/formation is not the cause of the current failure.

## Native 44.1 kHz characterization

`transport/amdtp44probe` established important facts at 44.1 kHz.

When the FW410 is switched to 44100 Hz and the host sends a continuous AMDTP NODATA stream with FDF `0x01`, the FW410 produces valid sample-bearing capture packets. Therefore all of these are already known to work at 44.1 kHz:

- AV/C rate selection
- CMP/ISO setup
- host TX channel/path
- device RX of host NODATA
- FDF `0x01`
- FW410 44.1 kHz clocking/capture
- host RX path

Observed FW410 capture at 44.1 kHz:

- FDF `0x01`
- DBS `5`
- data packets remain 168 bytes / 8 events
- 256 observed slots contained 176 data-bearing and 80 NODATA packets in one characterization run
- DBC advances by 8 on data packets and is retained across NODATA
- SYT follows a fractional/non-48k sequence

This is a crucial control case: **44.1 kHz itself works. The unresolved failure begins when the host transmits data-bearing 44.1 kHz playback packets.**

## Current unresolved issue: native 44.1 kHz PCM playback

Tool: `fw410/tools/transport/pcm44100playback/`

Current behavior after switching the FW410 to 44100 Hz:

- host builds a native 44.1 kHz prebuilt PCM TX program
- FDF = `0x01`
- DBS = `11`
- data packets contain 8 PCM events
- test tone is placed on Analog Output 1 / PCM position 2
- no sound is heard
- FW410 capture remains entirely NODATA (`data-bearing slots: 0`)

The current experimental TX program is 4096 FireWire cycles / 512 ms and uses a blocking base-44.1 SYT sequence. A representative schedule has 2823 data-bearing packets and 1273 NODATA packets.

### Things already eliminated

Do not restart these investigations from zero:

1. **Wrong sample-rate format:** eliminated. AV/C readback confirms 44100 Hz in both directions.
2. **Wrong FDF:** eliminated as a basic cause. FDF `0x01` is observed from the FW410 itself at 44.1 kHz and works in the NODATA control experiment.
3. **Wrong playback channel count / DBS:** eliminated. `formatprobe` confirms 10 PCM + 1 MIDI at 44.1 kHz, so DBS=11.
4. **NuDCL/CMP path generally broken at 44.1:** eliminated. `amdtp44probe` NODATA TX causes the device to enter sample-bearing 44.1 capture.
5. **Program construction missing the scheduled TX start cycle:** eliminated. The test lead was increased to 2048 cycles (256 ms), and immediately before `Start()` the measured scheduled cycle was still 1820 cycles ahead. The FW410 nevertheless remained NODATA.
6. **Simple 48-kHz PCM implementation problem:** not relevant; native 48 kHz PCM playback and live refill are confirmed working.

### Most useful current inference

The fault is now tightly isolated to **data-bearing 44.1 kHz host TX semantics**, especially cadence/phase/SYT/DBC behavior. Packet formation and general transport setup are substantially validated.

A particularly useful contrast is:

- known-good 44.1 control: continuous host NODATA -> FW410 emits sample-bearing capture
- failing case: host begins data-bearing 44.1 packets -> FW410 stays/returns NODATA and produces no playback sound

The next investigation should therefore avoid broad rewrites and compare the smallest possible difference between these cases.

### Recommended next experiment

Build an A/B 44.1 acceptance probe rather than another complete tone implementation:

1. Start with the exact known-good continuous NODATA TX behavior.
2. Confirm FW410 capture becomes sample-bearing.
3. Introduce one isolated correctly formed DBS=11 data packet into the otherwise-NODATA stream.
4. Observe whether capture remains sample-bearing or collapses to NODATA.
5. Then introduce a very small controlled sequence of data packets.
6. Vary only one dimension at a time: DBC convention, SYT/phase, or data cadence.

Another promising approach is to phase-lock host 44.1 playback timing to the FW410's observed 44.1 capture clock instead of free-running from an arbitrary initial phase. Linux BeBoB/AMDTP domain behavior should be consulted specifically for synchronization/phase and CIP flags, but its implementation should be treated as reference evidence rather than copied mechanically.

## Mixer/routing/headphone findings

Control work was intentionally paused to focus on PCM/CoreAudio, but important topology has been established.

`topologyprobe` mapped the BridgeCo/AV/C Audio and Music subunit topology, feature blocks, processing blocks, and selector blocks. `plugprobe` reports external output plugs including analog, digital, and MIDI endpoints.

Seven selector function blocks (FB1..FB7) respond to STATUS and initially report input plug 0.

A guarded `headphoneprobe` experiment changed selector FB7 from input 0 to input 1 temporarily and restored it afterward. During the test, playback channel 1 was heard in the headphone left channel and playback channel 2 in the headphone right channel. This confirms that headphone routing is controlled internally by the interface and that selector FB7 is relevant to the headphone path.

Leave this mapping as established evidence for now. A future interface control panel should expose routing/mixer/headphone controls after the PCM/CoreAudio path is mature.

## Architecture decisions to preserve

- Keep device/control/transport responsibilities separated.
- Continue extracting proven behavior into `fw410/lib` rather than leaving all logic in probes.
- Use Linux `snd-bebob` and FFADO to save reverse-engineering time, but do not assume Linux's implementation strategy is optimal on macOS.
- Preserve the manually validated NuDCL behavior when refactoring; abstractions must be verified against hardware.
- Prefer the FW410's native rate when possible. SRC is appropriate only when crossing different clock domains/rates.
- Preserve exact PCR values and restore them after experiments.
- Design eventual long-running streams for bus-reset/generation recovery.
- The end goal remains a reusable macOS FireWire audio DevKit, not a one-off FW410-only program.

## Suggested continuation order

1. Resolve native 44.1 kHz data-bearing TX using the NODATA-vs-data A/B probe and/or capture-clock phase synchronization.
2. Generalize the AMDTP scheduler so 44.1 and 48 kHz are native selectable rates rather than separate experimental implementations.
3. Return to CoreAudio integration and replace the current input-to-FW410 experiment with a real CoreAudio-facing playback/capture architecture.
4. Add robust rate switching and bus-reset recovery.
5. Later return to mixer/routing/headphone controls and build a user-facing FW410 control panel.
6. Add MIDI transport after the PCM stream architecture is stable.

## Quick handoff summary

If starting a new development session, the shortest accurate summary is:

> The FW410 user-space FireWire stack works. 48 kHz capture, PCM playback, live ring-buffer streaming, and a CoreAudio-input-to-FW410 bridge are hardware-confirmed. Sample-rate AV/C control works and 44.1 kHz NODATA duplex/capture works. Native 44.1 kHz **data-bearing host playback is the current blocker**: DBS=11/FDF=0x01/formation and TX start timing have already been validated or eliminated. Focus next on 44.1 data-packet DBC/SYT/cadence/phase semantics, preferably by modifying the known-good NODATA stream incrementally or synchronizing TX phase to the FW410 capture clock. Headphone routing via selector FB7 has also been confirmed but is intentionally deferred until after PCM/CoreAudio work.
