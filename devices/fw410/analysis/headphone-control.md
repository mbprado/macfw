# FW410 headphone control

Last updated: 2026-08-27

This document records the hardware-validated M-Audio FireWire 410 headphone-control model implemented by macfw on `feature/fw410-control-ipc`.

The Linux `snd-firewire-ctl-services` FW410 protocol implementation was used as a protocol reference and all controls described below were then validated on real FW410 hardware while macfw full-duplex playback/capture remained active.

## Control architecture

Standalone AV/C probes cannot safely open the FW410 while the production transport owns the FireWire interface. The validated architecture therefore keeps all hardware transactions inside the active native bridge and exposes controls over a local Unix-domain socket:

```text
fw410ctl / future GUI
        |
        | local control IPC
        v
/tmp/macfw-fw410-control.sock
        |
        v
active halbridge44100 / halbridge48000
        |
        | existing open IOFireWireLib handle + FCP response space
        v
M-Audio FireWire 410
```

This lets control transactions coexist with active playback, capture and software monitoring without stopping `haltransport` or opening the FireWire device a second time.

Control-socket startup is intentionally non-fatal: failure to create the control endpoint must not prevent audio transport from starting.

## Confirmed headphone signal flow

The validated headphone path is:

```text
                                +-----------------------------+
                                |  dedicated headphone mixer  |
                                |                             |
mixer-output 1/2  ------------>| 1/2   on/off                |
mixer-output 3/4  ------------>| 3/4   on/off                |
mixer-output 5/6  ------------>| 5/6   on/off                |
mixer-output 7/8  ------------>| 7/8   on/off                |
mixer-output 9/10 ------------>| 9/10  on/off                |
                                +--------------+--------------+
                                               |
                                               | mixer source
                                               v
                                      +-------------------+
aux-output 1/2 ---------------------->| headphone source  |
                                      | mixer / aux       |
                                      +---------+---------+
                                                |
                                                v
                                      +-------------------+
                                      | headphone volume  |
                                      | left / right      |
                                      +---------+---------+
                                                |
                                                v
                                           headphones
```

The AUX path is separate from the five-source headphone mixer. When `headphone-source = aux`, the headphone-mixer source switches can all be off and AUX remains audible.

## Headphone source selector

AV/C selector function block:

```text
FB 0x07
0 = mixer-output path
1 = aux-output-1/2 path
```

CLI:

```bash
fw410ctl headphone-source get
fw410ctl headphone-source set mixer
fw410ctl headphone-source set aux
```

Hardware validation:

- source changes work while playback/capture/monitoring remain active;
- `mixer` selects the dedicated five-source headphone mixer path;
- `aux` selects the independent AUX output path;
- AUX remains audible even when all five headphone-mixer sources are disabled.

## Headphone volume

AV/C Feature Function Block:

```text
FB 0x0f
channel 1 = headphone left
channel 2 = headphone right
control   = Volume
```

The AV/C level representation is signed 16-bit fixed-point with `0x0100` per dB:

```text
0x0000  = 0 dB
...
0x8000  = negative infinity
```

CLI:

```bash
fw410ctl headphone-volume get
fw410ctl headphone-volume set -12
fw410ctl headphone-volume set -20 0
fw410ctl headphone-volume set 0
```

Hardware validation:

- volume affects the headphone path for both `mixer` and `aux` source modes;
- equal L/R values provide normal linked stereo attenuation;
- independent left/right values work and reproduce balance/pan-like behavior;
- changing headphone volume does not alter host playback, host capture or software monitoring paths.

## Dedicated headphone mixer

The FW410 headphone mixer is an AV/C Processing Function Block at `0x07`.

It accepts five stereo mixer-output pairs:

```text
1/2
3/4
5/6
7/8
9/10
```

Each source is represented by a processing mixer coefficient:

```text
0x0000 = ON
0x8000 = OFF
```

CLI:

```bash
fw410ctl headphone-mixer get
fw410ctl headphone-mixer set 1/2 on
fw410ctl headphone-mixer set 1/2 off
fw410ctl headphone-mixer set 3/4 on
```

Hardware validation:

- all five switches read/write correctly;
- multiple source pairs can be enabled simultaneously;
- with all five sources off and `headphone-source = mixer`, headphones are silent;
- enabling only 3/4 and sending audio through outputs 3/4 produces the expected headphone audio;
- enabling 1/2 and sending audio through outputs 1/2 produces the expected headphone audio;
- this behavior matches the individual channel-pair choices in the original M-Audio FW410 control panel.

## AUX controls relevant to headphones

The FW410 AUX mixer is not a separate boolean processing matrix in the Linux protocol model. Its sources are controlled through per-source AV/C levels, followed by an AUX output level.

The first implemented AUX controls are:

```text
stream-input 1/2 source level: FB 0x06, channels 1/2
aux-output 1/2 level:          FB 0x09, channels 1/2
```

CLI:

```bash
fw410ctl aux-stream12-volume get
fw410ctl aux-stream12-volume set -12
fw410ctl aux-output-volume get
fw410ctl aux-output-volume set -12
```

On the tested hardware both initially read 0 dB. Selecting `headphone-source = aux` produced audible output independently of the headphone mixer switches, confirming the separate AUX path.

Further AUX-source controls for analog input, digital input and stream inputs 3-10 can reuse the same AV/C feature-level mechanism when the wider mixer/control-panel phase is implemented.

## Native-rate lifecycle validation

The complete headphone state was tested across runtime native sample-rate changes:

```text
44.1 kHz -> 48 kHz -> 44.1 kHz
48 kHz   -> 44.1 kHz -> 48 kHz
```

Confirmed preserved across rate switches:

- headphone source (`mixer` / `aux`);
- five headphone-mixer source states;
- headphone left/right volume values.

Playback, capture and monitoring continue to work through the transitions.

The transition from 48 kHz back to 44.1 kHz takes somewhat longer than the opposite direction. This is consistent with the already-known FW410 44.1 kHz startup path, which performs the post-start AV/C 44.1 reassertion before the native engine reports ready. No headphone-specific state regression was observed.

## Current CLI surface

```text
fw410ctl headphone-source get
fw410ctl headphone-source set mixer|aux

fw410ctl headphone-volume get
fw410ctl headphone-volume set <dB|-inf> [<right-dB|-inf>]

fw410ctl headphone-mixer get
fw410ctl headphone-mixer set 1/2|3/4|5/6|7/8|9/10 on|off

fw410ctl aux-stream12-volume get
fw410ctl aux-stream12-volume set <dB|-inf> [<right-dB|-inf>]

fw410ctl aux-output-volume get
fw410ctl aux-output-volume set <dB|-inf> [<right-dB|-inf>]
```

Volume range is currently exposed as `-128..0 dB` in 1 dB steps, plus `-inf` for AV/C negative infinity.

## Phase conclusion

The FW410 headphone subsystem is now understood well enough to be treated as an implemented control feature rather than an exploratory probe:

- live control IPC is proven at both 44.1 and 48 kHz;
- source selection is mapped and hardware-validated;
- independent stereo headphone volume is mapped and hardware-validated;
- the five-source dedicated headphone mixer is mapped and hardware-validated;
- the AUX path is confirmed independent from the five-source mixer;
- headphone state survives native sample-rate changes;
- control operations coexist with full-duplex audio without disturbing playback/capture.

The next logical development phase is a separate native macOS control-panel GUI using this IPC/control layer. Broader FW410 mixer/output/input mapping can then be added incrementally behind the same control API.
