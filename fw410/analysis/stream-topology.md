# FW410 operational stream topology

Confirmed on Intel macOS Monterey using the read-only `streamprobe` BridgeCo/BeBoB AV/C queries.

## Current operating state

- Product: `FW 410`
- GUID: `0x000d6c01005833e6`
- Current duplex sample rate: 48000 Hz
- Output and input plug 0 both report `IMPLEMENTED/STABLE`
- Both unit plug 0 endpoints report BridgeCo plug type `0x00` (isochronous)

## OUTPUT unit isochronous plug 0

Reported channel count: **5**

Sections:

1. S/PDIF (`type 0x04`), 2 channels
   - logical channel 1 -> AMDTP stream position 1
   - logical channel 2 -> AMDTP stream position 3
2. Line (`type 0x03`), 2 channels
   - logical channel 1 -> AMDTP stream position 2
   - logical channel 2 -> AMDTP stream position 4
3. MIDI conformant (`type 0x0a`), 1 channel
   - MIDI -> AMDTP stream position 5

Observed slot order at 48 kHz:

```text
position 1  S/PDIF 1
position 2  Line 1
position 3  S/PDIF 2
position 4  Line 2
position 5  MIDI
```

## INPUT unit isochronous plug 0

Reported channel count: **11**

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
6. MIDI conformant (`type 0x0a`), 1 channel
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

## Interpretation

This matches Linux `snd-bebob`'s channel-mapping model: BridgeCo channel-position data provides the AMDTP slot number and section-local location, while section type distinguishes PCM-like audio from MIDI-conformant data.

The reported counts are therefore useful directly for AMDTP packet layout: 4 audio slots + 1 MIDI slot in the OUTPUT direction, and 10 audio slots + 1 MIDI slot in the INPUT direction at the currently active 48 kHz formation.

No isochronous connection has been established yet. These results come entirely from AV/C STATUS discovery.