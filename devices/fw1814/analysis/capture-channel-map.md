# FW1814 capture channel map

Hardware-observed mapping for the M-Audio FireWire 1814 special firmware.

## Test baseline

- Sample rate: 48 kHz
- Clock: internal
- Digital mode: S/PDIF
- Capture transport: BeBoB/CIP blocking
- Data-bearing packet: DBS=11, FMT=0x10, FDF=0x02
- Physical mapping is assigned only from injected-signal tests; unknown positions remain unnamed.

## Confirmed capture positions

| AM824 position | Physical source | Evidence |
|---:|---|---|
| 0 | Analog Input 1 | 440 Hz sine injected only into physical Input 1. First run: RMS -18.3 dBFS, peak -8.8 dBFS; next-highest MBLA position about -76.5 dBFS (~58 dB separation). Louder repeat near clipping: pos 0 rose to RMS -11.7 dBFS and peak -2.7 dBFS while all other MBLA positions remained near the same ~-76 to -99 dBFS noise floor. The position therefore tracks the physical Input 1 level and is hardware-confirmed. |
| 1 | Analog Input 3 | Injected-signal mapping test. |
| 2 | Analog Input 5 | Injected-signal mapping test. |
| 3 | Analog Input 7 | Injected-signal mapping test. |
| 4 | Analog Input 2 | Injected-signal mapping test; observed as the dominant active position when physical Input 2 was driven. |
| 5 | Analog Input 4 | Injected-signal mapping test. |
| 6 | Analog Input 6 | Injected-signal mapping test. |
| 7 | Analog Input 8 | Injected-signal mapping test. |

The complete hardware-confirmed analog capture order is therefore:

```text
AM824 position:  0  1  2  3  4  5  6  7
Analog input:    1  3  5  7  2  4  6  8
```

Equivalently, by physical input number:

```text
Input 1 -> pos 0
Input 2 -> pos 4
Input 3 -> pos 1
Input 4 -> pos 5
Input 5 -> pos 2
Input 6 -> pos 6
Input 7 -> pos 3
Input 8 -> pos 7
```

This is the same odd-inputs-first / even-inputs-second style of AM824 channel ordering already encountered on the FW410 family.

## Still unassigned

- Positions 8-9 are the two remaining PCM positions in the 10-PCM S/PDIF capture formation. They were observed as `0x00000000` in the current tests because no digital source was available. Their exact left/right S/PDIF assignment remains unverified and must not be hardcoded yet.
- Position 10 was observed as `0x80000000`, consistent with the expected MIDI no-data position; CoreMIDI exposure remains deferred.

## Mapping procedure

Drive one known physical input at a time with a steady test tone, run `duplex-blocking-raw`, pipe the output through `fw1814_capture_map.py`, and assign the physical name only when one AM824 position shows an unambiguous activity increase over the others.
