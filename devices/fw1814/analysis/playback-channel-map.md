# FW1814 playback channel map

Hardware-observed mapping for the M-Audio FireWire 1814 special firmware.

## Test baseline

- Sample rate: 48 kHz
- Clock: internal
- Digital mode: S/PDIF
- Playback transport: BeBoB/CIP blocking
- Formation: 6 PCM + 1 MIDI, DBS=7
- Test signal: 500 Hz sine, -24 dBFS peak, inserted into one playback PCM position at a time
- Capture companion stream active during the mapping test

## First hardware pass

With no explicit special-firmware mixer routing programmed by macfw:

| Playback PCM position | Observed destination |
|---:|---|
| 0 | No audible analog output |
| 1 | No audible analog output |
| 2 | No audible analog output |
| 3 | No audible analog output |
| 4 | S/PDIF output |
| 5 | S/PDIF output |

This confirms positions 4 and 5 form the S/PDIF playback pair. Left/right ordering is not yet assigned because that orientation was not separately verified.

Positions 0-3 must not yet be labelled as physical analog outputs. FFADO's FW1814-specific special-firmware implementation documents an internal routing/mixer stage between FireWire Stream 1/2 + Stream 3/4 and Analog 1/2 + Analog 3/4. Therefore an otherwise-correct AMDTP stream can be silent at the analog jacks until the documented mixer routing is configured.

## Documented special-firmware routing relevant to playback

FFADO defines the special M-Audio control area as:

```text
MAUDIO_SPECIFIC_ADDRESS = 0xffc700000000
MAUDIO_CONTROL_OFFSET   = 0x00700000
```

The relevant write-only registers are:

```text
MIX_STM_IN    offset 0x94
SRC_ANA_OUT   offset 0x9c
```

For a straight stereo-pair mapping, FFADO's documented bit layout gives:

```text
MIX_STM_IN = 0x00000006
    Stream 1/2 -> Mixer 1/2
    Stream 3/4 -> Mixer 3/4

SRC_ANA_OUT = 0x00000000
    Analog 1/2 <- Mixer 1/2
    Analog 3/4 <- Mixer 3/4
```

The register area is write-only on FW1814/ProjectMix special firmware, so macfw must not attempt readback or unknown-register probing. A guarded diagnostic writes only these two documented values before repeating the physical position mapping.

## Deferred

- Exact mapping of positions 0-3 to Analog Outputs 1-4 remains pending the documented routing test.
- Exact S/PDIF L/R ordering for positions 4-5 remains deferred until cross-device digital testing is available.
- MIDI remains deferred.
