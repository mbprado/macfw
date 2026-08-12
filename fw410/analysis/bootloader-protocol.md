# FW410 boot-from-flash protocol

**Status:** Linux/FFADO correlation complete; hardware write not yet performed.

This note records the known M-Audio FireWire 410 boot-from-flash path used by Linux `snd-bebob` and correlated with the BeBoB/FreeBoB information-register layout used by FFADO.

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

Thus the 12 transmitted bytes are expected to be:

```text
01 00 00 00  00 00 11 01  00 00 00 00
```

Interpretation from the Linux source comments:

- cue 1: bootloader protocol version 1;
- cue 2: initialize configuration to factory settings (`0x1101`), command code zero, zero operands;
- cue 3: padding.

## Expected transition

After accepting the cue, the device is expected to load the application image from flash, generate a FireWire bus reset, and re-enumerate.

Expected identity transition:

```text
before: model 0x00010058 / "FW Bootloader"
after:  model 0x00010046 / operational FireWire 410 firmware
```

The node ID and bus generation must be treated as invalid after the reset and reacquired from the newly enumerated device.

## macfw implementation strategy

The first write-capable implementation should remain narrowly scoped. It should not expose arbitrary addresses or arbitrary payloads.

Before writing, it should verify:

1. current device is the FW410 bootloader personality;
2. BeBoB protocol version is `1`;
3. bootloader version is non-zero;
4. software date is at least `20070401`;
5. software/application ID is `0x00010046`;
6. target address is exactly `0xffffc8021000`;
7. payload is exactly the 12 bytes documented above.

After the write, it must release the old interface and wait for/re-discover the FireWire unit instead of continuing to use stale generation/node state.

## Current safe test

`fwprobe --boot-cue-check` performs the prerequisite checks and prints the exact would-be transaction, but performs **no write**.

Only after this dry run is validated on the physical FW410 should a one-shot boot command be added.
