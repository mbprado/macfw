# FW410 Reverse Engineering

This directory contains static-analysis artifacts and reports for the original M-Audio FireWire 410 driver.

## Analysis workflow

1. Preserve the original vendor artifact unchanged.
2. Extract the kext and Mach-O binary locally.
3. Record hashes and metadata.
4. Inspect `Info.plist` and IOKit personalities.
5. Extract strings and symbols.
6. Import the binary into Ghidra/Hopper/IDA.
7. Build a class/function map.
8. Correlate the macOS implementation with Linux BeBoB and FFADO.
9. Validate important behavior with hardware captures.

## Evidence status

| Area | Status |
|---|---|
| Mach-O architecture | Confirmed: x86_64 |
| Kext bundle metadata | Confirmed |
| FW410 IOKit personality | Confirmed |
| Legacy FireWire dependencies | Confirmed |
| Audio engine classes | Confirmed |
| Isochronous/DCL implementation | Confirmed |
| Firmware implementation | Confirmed |
| Complete protocol mapping | In progress |
| Sonoma transport architecture | Unknown |

See [`reports/initial-binary-analysis.md`](reports/initial-binary-analysis.md) for the first analysis report.
