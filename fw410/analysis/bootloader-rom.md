# FW410 bootloader configuration ROM

**Status:** Confirmed on hardware

This document records the FireWire configuration ROM exposed by an M-Audio FireWire 410 while it is in its bootloader personality. The data was collected on an Intel Mac running macOS Monterey using `fw410/tools/fwprobe --rom` and Apple's user-space `IOFireWireLib` interfaces. No vendor kext, FireWire writes, bus resets, or firmware commands were used.

## Observed device identity

```text
FireWire Product Name: FW Bootloader
Vendor_ID:              0x00000d6c
GUID:                   0x000d6c01105833e6
Unit_Spec_ID:           0x0000a02d
Unit_SW_Version:        0x00014001
```

The acquired `IOFireWireDeviceInterface` reported interface version 8. Bus generation and node ID are dynamic and are therefore not device identity fields.

## Unit configuration directory

The configuration directory reported type `0x11` with four entries:

| Index | Key | Type | Raw entry | Decoded value |
|---|---:|---|---:|---|
| 0 | `0x12` | immediate | `0x1200a02d` | `0x00a02d` |
| 1 | `0x13` | immediate | `0x13014001` | `0x014001` |
| 2 | `0x17` | immediate | `0x17010058` | `0x010058` |
| 3 | `0x01` | leaf | `0x8100000d` | `FW Bootloader` |

The textual descriptor leaf contains 24 bytes:

```text
00 00 00 00 00 00 00 00
46 57 20 42 6f 6f 74 6c
6f 61 64 65 72 00 00 00
```

which contains the ASCII string `FW Bootloader` after the descriptor header.

## Interpretation

The three immediate entries match standard IEEE 1394 unit-directory semantics used by the Linux FireWire stack:

- key `0x12`: unit specifier ID = `0x00a02d` (1394 Trade Association)
- key `0x13`: unit software version = `0x014001`
- key `0x17`: model ID = `0x010058`
- key `0x01`: textual descriptor = `FW Bootloader`

## Linux `snd-bebob` correlation

Linux's current `snd-bebob` device table contains two FW410 identities:

```c
SND_BEBOB_DEV_ENTRY(VEN_BRIDGECO, 0x00010058, NULL),
SND_BEBOB_DEV_ENTRY(VEN_BRIDGECO, 0x00010046, &maudio_fw410_spec),
```

The first entry has no normal device specification and is treated as a firmware-loader personality; the second is the normal FW410 firmware personality.

This confirms:

```text
FW410 bootloader model: 0x00010058
FW410 normal model:     0x00010046
specifier ID:           0x0000a02d
```

The Linux source notes that the FW410 configuration ROM may retain BridgeCo as the vendor field even though the product is M-Audio. The macOS registry on the tested unit separately reports M-Audio vendor ID `0x0d6c`, while the bootloader model ID agrees exactly with Linux.

## Linux firmware-loader protocol reference

Linux defines the BeBoB information and request-register bases as:

```text
BEBOB_ADDR_REG_INFO = 0xffffc8020000
BEBOB_ADDR_REG_REQ  = 0xffffc8021000
```

For M-Audio bootloaders, Linux first performs a **read-only** 8-byte read at information-register offset `0x20` to inspect the firmware build date. Only after validating that date does it send the three-quadlet firmware-load cue to the request register.

The cue is documented here for analysis only; `fwprobe` must not send it until the read path and protocol have been independently validated:

```text
0x00000001
0x01110000
0x00000000
```

Linux also documents that firmware version 5058 or later stores firmware in device flash and that this cue tells the bootloader to load it. The device then resets the FireWire bus and re-enumerates with the normal firmware personality.

## Next safe experiment

The next hardware operation should be a **read-only asynchronous block read** of 8 bytes from:

```text
0xffffc8020020
```

This corresponds to Linux `snd_bebob_maudio_load_firmware()` reading `INFO_OFFSET_SW_DATE` (`0x20`) relative to `BEBOB_ADDR_REG_INFO` (`0xffffc8020000`).

The result should be recorded as raw bytes first. Linux compares it against the ASCII date `20070401`; byte order must be verified from the actual macOS read before interpreting the value.

No write to `0xffffc8021000` should be implemented until this read succeeds and the old M-Audio driver / Linux behavior are cross-checked.
