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

## Initial hardware pass

With no explicit special-firmware mixer routing programmed by macfw:

| Playback PCM position | Observed destination |
|---:|---|
| 0 | No audible analog output |
| 1 | No audible analog output |
| 2 | No audible analog output |
| 3 | No audible analog output |
| 4 | S/PDIF output |
| 5 | S/PDIF output |

This confirmed positions 4 and 5 form the S/PDIF playback pair. Left/right ordering is intentionally unassigned until later cross-device digital testing.

Positions 0-3 were not directly audible because the FW1814 special firmware has an internal mixer/routing stage between the host playback streams and the analog outputs.

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

The guarded macfw routing test programmed only the documented straight-through values:

```text
MIX_STM_IN = 0x00000006
    Stream 1/2 -> Mixer 1/2
    Stream 3/4 -> Mixer 3/4

SRC_ANA_OUT = 0x00000000
    Analog 1/2 <- Mixer 1/2
    Analog 3/4 <- Mixer 3/4
```

The register area is write-only on FW1814/ProjectMix special firmware, so macfw must not attempt readback or unknown-register probing.

## Confirmed analog playback mapping

After applying the documented routing above, the hardware-observed mapping is:

| Playback PCM position | Physical analog output |
|---:|---:|
| 0 | Analog Output 3 |
| 1 | Analog Output 4 |
| 2 | Analog Output 1 |
| 3 | Analog Output 2 |

Equivalently, by physical output number:

```text
Analog Output 1 <- PCM position 2
Analog Output 2 <- PCM position 3
Analog Output 3 <- PCM position 0
Analog Output 4 <- PCM position 1
```

The complete currently confirmed S/PDIF-mode playback position map is therefore:

```text
PCM position 0 -> Analog Output 3
PCM position 1 -> Analog Output 4
PCM position 2 -> Analog Output 1
PCM position 3 -> Analog Output 2
PCM position 4 -> S/PDIF pair member (L/R not yet assigned)
PCM position 5 -> S/PDIF pair member (L/R not yet assigned)
```

## Headphone observation

During the same routed playback mapping, PCM positions 0 and 1 were also audible on the headphone output, one position in each headphone stereo channel.

This is recorded as a hardware observation only. The routing diagnostic did not program the documented headphone-source register at offset `0x98`, so the current headphone behavior may depend on mixer state already present in the unit. Do not hardcode a permanent headphone routing assumption from this observation yet.

## Intermittent startup observation

The playback mapper occasionally reaches the proven duplex startup sequence but reports:

```text
reassert OUTPUT 48000 Hz... PASS
waiting 100 ms before INPUT rate CONTROL...
reassert INPUT 48000 Hz... FAIL
```

PCR restoration succeeds afterward. In the observed failure there was no FCP write-failure or response-timeout message.

A strong software-side race hypothesis now exists: the current FCP response callback marks any response from the expected FW1814 node as the response for the active transaction, while `setSignalRate()` also accepts AV/C `INTERIM` (`0x0f`) as a successful terminal response. If OUTPUT returns `INTERIM` first and its later final response arrives after the INPUT transaction has reset the shared response context, that late OUTPUT response can be mistaken for the INPUT response. The INPUT validator then sees opcode `0x18` instead of `0x19` and reports `FAIL` even though the device may have accepted the INPUT command normally.

This matches the intermittent nature of the failure and the absence of an FCP timeout. The production FCP helper should therefore match responses to the active command and handle `INTERIM` as non-final, waiting for the corresponding final response instead of treating the first response from the node as terminal. Before changing transport timing or command bytes, capture the exact response on a failing run to verify this race.

## Deferred

- Exact S/PDIF L/R ordering for positions 4-5 remains deferred until cross-device digital testing is available.
- Exact programmable headphone routing remains deferred until the analog CoreAudio path is stable.
- MIDI remains deferred.
