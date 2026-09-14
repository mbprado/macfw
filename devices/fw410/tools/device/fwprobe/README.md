# fwprobe

Minimal user-space FireWire diagnostic for M1.

The probe intentionally does **not** open the FW410 for exclusive access except briefly around narrowly scoped direct FireWire transactions, and does not perform arbitrary writes. It discovers FireWire services, obtains `IOFireWireDeviceInterface`, reports bus generation / node information, and can optionally inspect the device configuration ROM or perform read-only BeBoB register probes.

## Build

```bash
make
```

## Basic probe

```bash
./fwprobe
```

## Configuration ROM inspection

```bash
./fwprobe --rom
```

This uses `IOFireWireConfigDirectoryInterface` to enumerate the remote configuration ROM. It is read-only.

## BeBoB software build date probe

```bash
./fwprobe --info-date
```

This performs one read-only 8-byte transaction at `0xffffc8020020`. Linux `snd-bebob` uses the same field before it considers sending the M-Audio firmware-loader cue.

Confirmed on the FW410 bootloader under Intel macOS Monterey:

```text
raw:   32 30 30 37 30 35 30 34
ASCII: 20070504
```

## Full BeBoB information-register probe

```bash
./fwprobe --info
```

FFADO/FreeBoB documents the BeBoB bootloader information block at `0xffffc8020000`. `--info` performs one read-only 104-byte transaction covering offsets `0x00` through `0x67` and decodes the documented fields.

On the tested Intel Mac, numeric fields in this BeBoB information structure must be interpreted using the same little-endian native layout used by FFADO on Linux. This is distinct from the ASCII date/time fields, which are shown directly. The GUID is displayed using FFADO's two-32-bit-word layout.

Confirmed FW410 values include:

```text
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

The `software ID` value `0x00010046` matches the normal FW410 model ID used by Linux `snd-bebob`, providing strong evidence that the application firmware is already present in flash.

## M-Audio boot-from-flash cue check

```bash
./fwprobe --boot-cue-check
```

This mode is a **dry run**. It re-reads the BeBoB information block and verifies the prerequisites used for the known FW410 flash-boot path:

- BeBoB bootloader protocol version 1;
- non-zero bootloader version;
- software build date at least `20070401`;
- application/software ID `0x00010046`.

If all checks pass, it prints the Linux FW410 cue that would be written to the BeBoB request register:

```text
target:           0xffffc8021000
logical quadlets: 0x00000001 0x01110000 0x00000000
wire bytes:       01 00 00 00 00 00 11 01 00 00 00 00
```

**No write is performed.**

Linux `snd-bebob` writes these three little-endian quadlets as one block to request the bootloader to start the firmware stored in flash. The expected result is a FireWire bus reset followed by re-enumeration from bootloader model `0x00010058` to normal FW410 model `0x00010046`.

## Safety

`--rom`, `--info-date`, `--info`, and `--boot-cue-check` do not perform FireWire writes. They do **not**:

- reset the FireWire bus;
- upload firmware;
- start firmware;
- send the M-Audio bootloader cue;
- allocate isochronous channels;
- start audio or MIDI streaming.

## Confirmed FW410 bootloader identity

On Intel macOS Monterey, `--rom` decoded:

```text
specifier:        0x00a02d
software version: 0x014001
model:            0x010058
text descriptor:  FW Bootloader
```

This matches the Linux `snd-bebob` FW410 bootloader identity. See [`../../analysis/bootloader-rom.md`](../../analysis/bootloader-rom.md).

## Help

```bash
./fwprobe --help
```

## Current M1 progress

1. [x] Discover `IOFireWireUnit` services.
2. [x] Obtain `IOFireWireDeviceInterface` from user space.
3. [x] Read bus generation and remote node ID.
4. [x] Validate read-only configuration-ROM inspection on FW410 hardware.
5. [x] Validate bootloader identity against Linux `snd-bebob`.
6. [x] Validate direct BeBoB register access (`--info-date`, result `20070504`).
7. [x] Add and validate a full documented BeBoB information-register probe (`--info`).
8. [x] Correct numeric BeBoB field interpretation against FFADO/Linux semantics.
9. [x] Add a no-write FW410 boot cue prerequisite check (`--boot-cue-check`).
10. [ ] Validate `--boot-cue-check` on FW410 hardware.
11. [ ] After validation, implement an explicitly gated one-shot boot-from-flash cue and re-enumeration watcher.
12. [ ] Add isochronous capability probing.
13. [ ] Repeat successful probes on Intel macOS Sonoma.

Do not add arbitrary write operations. Any future write mode must be limited to a command independently verified against trusted implementations and must clearly state the expected hardware transition before execution.
