# FW410 current development status

Last updated: 2026-08-16

This is the handoff document for continuing development in a fresh session. It records what is confirmed on real FW410 hardware, the current architecture, and the next integration work. Treat the Linux `snd-bebob`/FireWire implementation and FFADO as protocol references, not code that must be copied blindly: prefer a cleaner/macOS-native implementation when appropriate.

## Major breakthrough: native 44.1 kHz playback works

Native 44.1 kHz host playback is now hardware-confirmed working on the M-Audio FireWire 410.

The previously failing packet scheduler was not the root cause. The decisive requirement is the M-Audio-specific stream-start sequence also reflected in Linux `snd-bebob` behavior:

1. Switch both FW410 signal-format directions to 44100 Hz.
2. Establish both CMP connections.
3. Start duplex ISO streaming.
4. Send valid 44.1 kHz AMDTP NODATA first.
5. While the duplex stream is live, reassert the AV/C sample rate on both OUTPUT plug 0 and INPUT plug 0.
6. Continue into the normal native blocking 44.1 kHz data-bearing schedule.

The successful hardware run used 512 NODATA cycles (64 ms), with the AV/C 44100 reassertion approximately 20 ms after NODATA transmission actually began. A 1 kHz tone was then heard on Analog Output 1.

Representative confirmed result:

- AV/C OUTPUT 44100 reassert: accepted
- AV/C INPUT 44100 reassert: accepted
- capture slots touched: 256 / 256
- capture data-bearing slots: 176
- capture FDF=0x01 slots: 256
- audible 1 kHz tone on Analog Output 1

This proves that the existing native 44.1 packet semantics are accepted once the FW410 receives its required M-Audio startup ritual.

The diagnostic proof tool is `fw410/tools/transport/pcm44100warmup/`.

The normal `fw410/tools/transport/pcm44100playback/run44100.sh` currently routes through that hardware-proven startup path. The next engineering step is to move the M-Audio post-start AV/C reassertion and initial NODATA phase into reusable transport/control infrastructure instead of leaving the behavior probe-specific.

## Test environment

Confirmed hardware testing has been performed on an Intel Mac with the M-Audio FireWire 410 using Apple's user-space `IOFireWireLib` path. The FW410 can be booted from its bootloader personality into the operational `FW 410` personality entirely from user space.

## Repository/tool organization

Tools are intentionally separated by responsibility:

- `fw410/tools/device/` — device/FireWire discovery and boot-related tools.
- `fw410/tools/control/` — AV/C, topology, plug, mixer, headphone, and sample-rate controls.
- `fw410/tools/transport/` — CMP, isochronous, AMDTP, PCM streaming, and CoreAudio bridge experiments.
- `fw410/lib/` — reusable transport/audio components extracted from successful experiments.

The control/transport split is deliberate. A future FW410 control-panel application can be built on the control work without mixing it into the PCM/ISO engine.

## Confirmed 48 kHz transport and PCM

48 kHz remains the known-good reference path.

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

The earlier bridge used FW410 transport at 48 kHz plus linear 44.1 -> 48 kHz SRC because native FW410 44.1 playback had not yet been solved. Since native 44.1 playback is now confirmed, the next CoreAudio milestone is a native 44.1 kHz bridge path with no SRC when both CoreAudio and FW410 use 44100 Hz.

Confirmed successful bridge characteristics included:

- CoreAudio input native rate: 44100 Hz
- AUHAL client: 44100 Hz mono Float32
- FW410 transport in the original bridge: 48000 Hz
- bridge SRC: 44100 -> 48000 linear
- actual microphone audio heard through FW410 outputs 1 and 2
- zero CoreAudio render errors
- zero PCM dropped frames in the successful test

## Sample-rate control

`control/rateprobe` can read and change the FW410 signal format in both directions. A 48 kHz -> 44.1 kHz transition has been confirmed by AV/C readback on both device OUTPUT/host-capture and device INPUT/host-playback plugs.

Changing sample rate can trigger a FireWire generation/node change, so tools must not assume that the original generation/node remains valid indefinitely. Reacquisition after reset remains an architectural requirement for the eventual long-running driver.

A critical M-Audio quirk is now hardware-confirmed: after duplex ISO is live, the 44100 signal-format command must be reasserted on both directions before the FW410 begins normal sample-bearing 44.1 operation.

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

For 44.1 kHz playback, the formation remains **10 PCM + 1 MIDI**, therefore **DBS=11 is confirmed correct**.

## Native 44.1 kHz characterization

`transport/amdtp44probe` established the 44.1 clock/cadence control case.

When the FW410 is switched to 44100 Hz and the host sends continuous AMDTP NODATA with FDF `0x01`, the FW410 produces valid sample-bearing capture packets.

Observed FW410 capture at 44.1 kHz:

- FDF `0x01`
- DBS `5`
- data packets remain 168 bytes / 8 events
- a representative 256-slot run produced 176 data-bearing and 80 NODATA packets
- DBC advances by 8 on data packets and is retained across NODATA
- SYT follows the expected fractional base-44.1 sequence

The 44.1 playback scheduler uses the blocking base-44.1 SYT sequence corresponding to Linux's initial state (`last_syt_offset = 3072`, base-44.1 SYT state 67). Once the M-Audio post-start AV/C reassertion is performed, this scheduler produces audible playback and the FW410 emits normal sample-bearing capture.

## What the failed 44.1 experiments taught us

The earlier failure was useful because it eliminated several false leads:

1. AV/C rate selection was correct.
2. FDF `0x01` was correct.
3. DBS `11` / 10 PCM + 1 MIDI was correct.
4. CMP and NuDCL paths worked at 44.1.
5. TX start lead was sufficient.
6. The base-44.1 DBC/SYT/cadence implementation was fundamentally correct.
7. A simple NODATA warm-up alone was insufficient.
8. The missing action was the M-Audio-specific **post-start AV/C rate reassertion while duplex AMDTP was already live**.

Do not reopen those eliminated hypotheses unless new hardware evidence contradicts them.

## Mixer/routing/headphone findings

Control work was intentionally paused to focus on PCM/CoreAudio, but important topology has been established.

`topologyprobe` mapped the BridgeCo/AV/C Audio and Music subunit topology, feature blocks, processing blocks, and selector blocks. `plugprobe` reports external output plugs including analog, digital, and MIDI endpoints.

Seven selector function blocks (FB1..FB7) respond to STATUS and initially report input plug 0.

A guarded `headphoneprobe` experiment changed selector FB7 from input 0 to input 1 temporarily and restored it afterward. During the test, playback channel 1 was heard in the headphone left channel and playback channel 2 in the headphone right channel. This confirms that headphone routing is controlled internally by the interface and that selector FB7 is relevant to the headphone path.

## Architecture decisions to preserve

- Keep device/control/transport responsibilities separated.
- Continue extracting proven behavior into `fw410/lib` rather than leaving all logic in probes.
- Model the M-Audio post-start AV/C rate reassertion as a device/startup quirk, not as part of generic AMDTP packet generation.
- Keep the initial NODATA/startup phase explicit in the stream-start state machine.
- Use Linux `snd-bebob` and FFADO to save reverse-engineering time, but do not assume Linux's implementation strategy is optimal on macOS.
- Preserve the manually validated NuDCL behavior when refactoring; abstractions must be verified against hardware.
- Prefer the FW410's native rate when possible. SRC is appropriate only when crossing different clock domains/rates.
- Preserve exact PCR values and restore them after experiments.
- Design eventual long-running streams for bus-reset/generation recovery.
- The end goal remains a reusable macOS FireWire audio DevKit, not a one-off FW410-only program.

## Suggested continuation order

1. Extract the successful 44.1 startup sequence into reusable code: initial NODATA phase + M-Audio post-start AV/C rate reassertion.
2. Extend the reusable live PCM stream engine to native 44.1 kHz instead of only prebuilt tone playback.
3. Run the CoreAudio bridge natively at 44.1 kHz without SRC when both endpoints use 44100 Hz.
4. Generalize the AMDTP scheduler so 44.1 and 48 kHz are selectable rates in the same stream architecture.
5. Add robust rate switching and bus-reset/generation recovery.
6. Later return to mixer/routing/headphone controls and MIDI transport.

## Quick handoff summary

> The FW410 user-space FireWire stack works. 48 kHz capture/playback/live streaming and the CoreAudio bridge are hardware-confirmed. Native 44.1 kHz playback is also now hardware-confirmed: the existing DBS=11/FDF=0x01/base-44.1 packet scheduler works, but M-Audio firmware requires duplex AMDTP to be live with an initial NODATA phase and then requires the 44100 AV/C signal-format command to be reasserted on both OUTPUT and INPUT plugs. With that sequence, a 1 kHz tone is audible on Analog Output 1 and capture returns to its normal 176/256 sample-bearing cadence. Next, move this startup quirk into reusable stream infrastructure and build native 44.1 live/CoreAudio streaming.
