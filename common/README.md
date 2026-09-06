# macfw common

`common/` is the device-independent part of macfw.

The FW410 implementation proved the CoreAudio/FireWire architecture before the repository had a second supported interface. As additional interfaces are added, reusable pieces are migrated here incrementally instead of cloning an entire device tree and allowing the implementations to diverge.

## Intended scope

Reusable code belongs here when its behavior is not specific to one hardware model:

- IOFireWireLib device/session handling;
- configuration-ROM and BeBoB information-register helpers;
- CMP/IRM allocation and connection management;
- CIP/AM824/AMDTP packet handling;
- NuDCL receive/transmit infrastructure;
- PCM ring buffers and CoreAudio shared-memory transport primitives;
- transport status/recovery primitives;
- generic real-time scheduling helpers;
- common device identity/profile types;
- common diagnostics infrastructure.

Device-specific stream geometry, channel maps, startup quirks, clock rules, mixer/control protocols, HAL presentation and GUI layout belong under `devices/<id>/`.

## Migration rule

The released FW410 implementation under `fw410/` remains the regression reference while this layout is introduced. Generic pieces should be moved into `common/` only when a second device needs them and the move can be validated without changing FW410 behavior.

This avoids a large path-only refactor before FW1814 bring-up and gives each extraction a concrete cross-device requirement.
