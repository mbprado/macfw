# FW410 operational stream topology

Confirmed on Intel macOS Monterey using the read-only `streamprobe` and `formatprobe` BridgeCo/BeBoB AV/C queries.

## Direction convention

BridgeCo/AV/C plug directions are **device-relative**:

- BridgeCo `OUTPUT` means data transmitted **out of the FW410** and received by the host. In CoreAudio terms this is the interface's **input/capture** stream.
- BridgeCo `INPUT` means data received **into the FW410** from the host. In CoreAudio terms this is the interface's **output/playback** stream.

This matches Linux `snd-bebob`: the device's CMP OUTPUT connection is paired with an `AMDTP_IN_STREAM` on the host, while the device's CMP INPUT connection is paired with an `AMDTP_OUT_STREAM` on the host.

## Supported host-facing format matrix

`formatprobe` successfully enumerated six BridgeCo stream-format entries in both directions. Entry 6 is rejected, which terminates the list.

| Sample rate | Host capture/input | Host playback/output | MIDI |
|---|---:|---:|---:|
| 44.1 kHz | 4 PCM | 10 PCM | 1 each direction |
| 48 kHz | 4 PCM | 10 PCM | 1 each direction |
| 88.2 kHz | 4 PCM | 10 PCM | 1 each direction |
| 96 kHz | 4 PCM | 10 PCM | 1 each direction |
| 176.4 kHz | 2 PCM | 8 PCM | 1 each direction |
| 192 kHz | 2 PCM | 8 PCM | 1 each direction |

No 32 kHz formation is advertised by the tested FW410 firmware.

The BridgeCo formation payload uses MBLA/PCM cluster code `0x06` and MIDI-conformant cluster code `0x0d`. The reduced payload at 176.4/192 kHz corresponds to the reduced PCM counts above.

## Current operating state

- Product: `FW 410`
- GUID: `0x000d6c01005833e6`
- Current duplex sample rate: 48000 Hz
- Output and input plug 0 both report `IMPLEMENTED/STABLE`
- Both unit plug 0 endpoints report BridgeCo plug type `0x00` (isochronous)

## BridgeCo OUTPUT unit isochronous plug 0

**Host perspective: input / capture stream**

Reported AMDTP data-channel count at 48 kHz: **5**

Sections:

1. S/PDIF (`type 0x04`), 2 channels
   - logical channel 1 -> AMDTP stream position 1
   - logical channel 2 -> AMDTP stream position 3
2. Line (`type 0x03`), 2 channels
   - logical channel 1 -> AMDTP stream position 2
   - logical channel 2 -> AMDTP stream position 4
3. MIDI conformant (`type 0x0a`), 1 data channel
   - MIDI -> AMDTP stream position 5

Observed slot order at 48 kHz:

```text
position 1  S/PDIF 1
position 2  Line 1
position 3  S/PDIF 2
position 4  Line 2
position 5  MIDI
```

So from the Mac/CoreAudio point of view this is **4 audio input channels**, plus one MIDI data slot in the AMDTP stream.

## BridgeCo INPUT unit isochronous plug 0

**Host perspective: output / playback stream**

Reported AMDTP data-channel count at 48 kHz: **11**

Sections:

1. S/PDIF (`type 0x04`), 2 channels
   - positions 1 and 6
2. Line1 (`type 0x03`), 2 channels
   - positions 2 and 7
3. Line2 (`type 0x03`), 2 channels
   - positions 3 and 8
4. Line3 (`type 0x03`), 2 channels
   - positions 4 and 9
5. Line4 (`type 0x03`), 2 channels
   - positions 5 and 10
6. MIDI conformant (`type 0x0a`), 1 data channel
   - position 11

Observed slot order at 48 kHz:

```text
position  1  S/PDIF 1
position  2  Line1 1
position  3  Line2 1
position  4  Line3 1
position  5  Line4 1
position  6  S/PDIF 2
position  7  Line1 2
position  8  Line2 2
position  9  Line3 2
position 10  Line4 2
position 11  MIDI
```

So from the Mac/CoreAudio point of view this is **10 audio output channels**, plus one MIDI data slot in the AMDTP stream.

## Interpretation

This matches Linux `snd-bebob`'s channel-mapping model: BridgeCo channel-position data provides the AMDTP slot number and section-local location, while section type distinguishes PCM-like audio from MIDI-conformant data.

The important distinction is that BridgeCo `OUTPUT`/`INPUT` describe the **FW410 endpoint direction**, while user-facing audio APIs normally describe direction relative to the **computer**.

No isochronous connection has been established yet. These results come entirely from AV/C STATUS discovery.