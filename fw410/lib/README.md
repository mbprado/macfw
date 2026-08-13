# FW410 reusable backend

This directory is the reusable user-space backend being extracted from the
proven FW410 diagnostic tools.

The extraction is deliberately incremental. The working tools remain the
regression suite while reusable pieces move here one subsystem at a time.

## Current layout

```text
lib/
├── include/macfw/
│   ├── am824.h
│   └── channel_map.h
├── src/
│   └── smoke.cpp
└── Makefile
```

## Extracted so far

- confirmed FW410 48 kHz capture channel map
- physically validated 48 kHz playback/output map
- proven AM824 MBLA24 capture decode/statistics helper
- standalone compile/smoke check

Run:

```bash
cd fw410/lib
make check
```

## Planned extraction order

1. channel maps and AM824 packet helpers
2. FireWire device/session wrapper
3. CMP/IRM connection management
4. BeBoB/AV/C discovery and clock/sample-rate control
5. AMDTP receive/transmit transport
6. FW410-specific boot/application-state handling
7. macOS audio-facing layer

`tools/` remains the diagnostic/regression suite during the migration.
