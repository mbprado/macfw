# FW410 reusable backend

This directory is the beginning of the reusable user-space backend extracted from the proven FW410 diagnostic tools.

The initial extraction is deliberately conservative: only side-effect-free protocol knowledge and AM824 helpers live here. The working tools remain unchanged until each subsystem can be migrated and re-tested independently.

## Layout

```text
lib/
├── include/macfw/
│   ├── am824.h
│   └── channel_map.h
├── src/
│   └── channel_map.cpp
└── Makefile
```

## Current scope

- Confirmed FW410 48 kHz capture/playback channel maps.
- AM824 MBLA24 decode/statistics helpers.
- A small `libmacfw.a` target to give later CMP, FireWire-device, BeBoB, and AMDTP transport code a stable home.

## Planned extraction order

1. channel maps and packet helpers
2. FireWire device/session wrapper
3. CMP/IRM connection management
4. BeBoB/AV/C discovery and clock/sample-rate control
5. AMDTP receive/transmit transport
6. FW410-specific boot/application state handling
7. macOS audio-facing layer

The `tools/` directory remains the regression/diagnostic suite throughout this migration.
