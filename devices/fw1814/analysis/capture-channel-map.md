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
| 0 | Analog Input 1 | 440 Hz sine injected only into physical Input 1; pos 0 measured RMS -18.3 dBFS and peak -8.8 dBFS while the next-highest MBLA position was about -76.5 dBFS (~58 dB separation). |

## Still unassigned

- Positions 1-7 carry MBLA-labelled (`0x40`) words in the current S/PDIF baseline and require individual physical-input tests.
- Positions 8-9 were observed as `0x00000000` in the initial raw captures; do not assign a meaning yet.
- Position 10 was observed as `0x80000000`, consistent with the expected MIDI no-data position; CoreMIDI exposure remains deferred.

## Mapping procedure

Drive one known physical input at a time with a steady test tone, run `duplex-blocking-raw`, pipe the output through `fw1814_capture_map.py`, and assign the physical name only when one AM824 position shows an unambiguous activity increase over the others.
