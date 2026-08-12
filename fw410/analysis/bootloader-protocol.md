# FW410 boot-from-flash protocol

**Status:** Confirmed on physical FW410 hardware from macOS user space.

This note records the M-Audio FireWire 410 boot-from-flash path used by Linux `snd-bebob`, correlated with FFADO/FreeBoB, and now reproduced successfully on Intel macOS Monterey with `IOFireWireLib`.

## Confirmed bootloader identity

Before the boot cue, the tested unit exposed:

```text
product:          FW Bootloader
model ID:         0x00010058
specifier ID:     0x0000a02d
unit SW version:  0x00014001
bus generation:   144
```

The BeBoB information block at `0xffffc8020000` was readable from user space and reported:

```text
manufacturer:         bridgeCo
protocol version:     0x00000001
bootloader version:   0x00002705
GUID:                 0x000d6c01005833e6
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

`software ID = 0x00010046` matches the normal FW410 model identity used by Linux and indicates that the operational application image is present in device flash.

## BeBoB addresses

Linux `snd-bebob` defines:

```text
information register: 0xffffc8020000
request register:     0xffffc8021000
```

FFADO additionally documents:

```text
request buffer:       0xffffc8021040
response register:    0xffffc8029000
response buffer:      0xffffc8029040
```

The FW410 flash-boot shortcut does not upload an image through these buffers. Linux sends a direct 12-byte cue to the request register.

## Boot cue

The logical values are:

```text
0x00000001
0x01110000
0x00000000
```

The 12 bytes sent by `fwboot` are:

```text
01 00 00 00  00 00 11 01  00 00 00 00
```

to:

```text
0xffffc8021000
```

`fwboot` performs a live preflight before the write and requires:

1. BeBoB protocol version `1`;
2. non-zero bootloader version;
3. software date >= `20070401`;
4. application/software ID `0x00010046`.

## Confirmed hardware execution

The guarded command:

```bash
./fwboot --execute
```

completed the single 12-byte block write successfully.

Immediately afterward, the FireWire bus reset and the device re-enumerated with bus generation `145`.

### Identity transition

Before:

```text
FireWire Product Name: FW Bootloader
GUID:                  0x000d6c01105833e6
model ID:              0x00010058
bootloader version:    0x00002705
bus generation:        144
```

After:

```text
FireWire Product Name: FW 410
GUID:                  0x000d6c01005833e6
model ID:              0x00010046
bootloader version:    0x00000000
bus generation:        145
```

The operational configuration-ROM descriptor became `FW 410`, and the unit model entry changed from `0x10058` to `0x10046`.

This confirms the complete bootloader-to-operational transition from an ordinary macOS user-space process without loading the original M-Audio kernel extension.

## Architectural consequence

Firmware startup is no longer a blocker for the user-space design. `macfw` can discover a powered FW410 in bootloader mode, inspect it, start the existing application firmware from flash, survive the resulting bus reset by discarding stale generation/node state, and rediscover the operational FW410.

The project can now move to operational-device protocol discovery: AV/C/FCP, plugs, clock/sample-rate controls, MIDI/control endpoints, and then isochronous audio streaming.
