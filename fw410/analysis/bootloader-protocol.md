# FW410 boot-from-flash protocol

**Status:** Linux/FFADO correlation complete; hardware cue write confirmed on Intel macOS Monterey.

This note records the M-Audio FireWire 410 boot-from-flash path used by Linux `snd-bebob`, correlated with the BeBoB/FreeBoB information-register layout used by FFADO, and now confirmed for the cue write on physical FW410 hardware from macOS user space.

## Confirmed hardware state

The tested FW410 powers up as:

```text
product:          FW Bootloader
model ID:         0x00010058
specifier ID:     0x0000a02d
unit SW version:  0x00014001
```

The BeBoB information block at `0xffffc8020000` is readable from macOS user space through `IOFireWireLib`.

Confirmed fields include:

```text
manufacturer:         bridgeCo
protocol version:     0x00000001
bootloader version:   0x00002705
hardware model ID:    0x00000002
hardware revision:    0x00000001
software date:        20070504
software time:        102656
software ID:          0x00010046
software version:     0x00ffffff
base address:         0x20080000
max image length:     0x00180000
bootloader date:      20030404
bootloader time:      134625
```

`software ID = 0x00010046` matches the normal FW410 model identity used by Linux, strongly indicating that the operational application image is already present in device flash.

## BeBoB addresses

Linux `snd-bebob` defines:

```text
information register: 0xffffc8020000
request register:     0xffffc8021000
```

FFADO additionally documents request/response buffer locations used by the general BeBoB download protocol:

```text
request buffer:       0xffffc8021040
response register:    0xffffc8029000
response buffer:      0xffffc8029040
```

The FW410 flash-boot shortcut does not upload an image through these buffers. Linux uses a direct 12-byte cue to the request register.

## Linux FW410 flash-boot prerequisite

Linux first reads eight bytes at information-register offset `0x20` and requires a software date at least:

```text
20070401
```

The tested FW410 reports:

```text
20070504
```

Linux documents that firmware version 5058 or later is stored in flash and can be started by sending a cue once.

## Boot cue

The logical cue values are:

```text
0x00000001
0x01110000
0x00000000
```

Linux converts each value with `cpu_to_le32()` and sends the three quadlets in one block transaction to:

```text
0xffffc8021000
```

Thus the 12 transmitted bytes are:

```text
01 00 00 00  00 00 11 01  00 00 00 00
```

Interpretation from the Linux source comments:

- cue 1: bootloader protocol version 1;
- cue 2: initialize configuration to factory settings (`0x1101`), command code zero, zero operands;
- cue 3: padding.

## Confirmed macOS hardware write

On Intel macOS Monterey, `fw410/tools/fwboot/fwboot --execute` passed all live preflight checks and performed exactly one 12-byte write to `0xffffc8021000`.

Observed result:

```text
preflight:
  protocol v1:       PASS (0x1)
  bootloader active: PASS (0x2705)
  software date:     PASS (20070504)
  FW410 app ID:      PASS (0x10046)
executing one-shot boot cue:
  address:    0xffffc8021000
  bytes:      01 00 00 00 00 00 11 01 00 00 00 00
write result: success (12 bytes)
```

This confirms that the documented Linux boot cue can be issued successfully from macOS user space through `IOFireWireLib` without the original M-Audio kernel extension.

The next validation is re-enumeration: confirm that the old bootloader unit disappears, the FireWire bus generation changes, and a new operational FW410 unit appears with model `0x00010046`.

## Expected transition

After accepting the cue, the device is expected to load the application image from flash, generate a FireWire bus reset, and re-enumerate.

Expected identity transition:

```text
before: model 0x00010058 / "FW Bootloader"
after:  model 0x00010046 / operational FireWire 410 firmware
```

The node ID and bus generation must be treated as invalid after the reset and reacquired from the newly enumerated device.

## macfw implementation strategy

Write-capable operations remain narrowly scoped. `fwprobe` stays read-only. `fwboot` performs only the documented FW410 flash-boot cue and only after verifying the exact known bootloader/application state.

The next milestone is to characterize the operational firmware after re-enumeration: configuration ROM, BeBoB info registers, AV/C subunits/plugs, and eventually isochronous stream capabilities.
