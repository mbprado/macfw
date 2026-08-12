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

FFADO/FreeBoB documents the BeBoB bootloader information block at `0xffffc8020000`. `--info` performs one read-only 104-byte transaction covering offsets `0x00` through `0x67` and decodes the documented fields:

- manufacturer ID;
- bootloader protocol version;
- bootloader version;
- device GUID;
- hardware model ID and revision;
- software date and time;
- software ID and version;
- application base address;
- maximum image length;
- bootloader date and time;
- debugger date/time, ID, and version.

Numeric fields are decoded from the FireWire byte stream as big-endian values. ASCII date/time fields are also displayed directly so the raw device representation remains easy to verify.

The FireWire interface is opened only for the duration of the direct transaction and closed immediately afterward.

## Safety

`--rom`, `--info-date`, and `--info` are read-only. They do **not**:

- issue FireWire writes;
- reset the FireWire bus;
- upload or start firmware;
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
7. [x] Add a full documented read-only BeBoB information-register probe (`--info`).
8. [ ] Validate `--info` on FW410 hardware and record the returned fields.
9. [ ] Map the M-Audio bootloader cue and expected re-enumeration without sending it yet.
10. [ ] Add isochronous capability probing.
11. [ ] Repeat successful probes on Intel macOS Sonoma.

Do not add arbitrary write operations until the exact FW410 command has been verified against Linux/FFADO and the original M-Audio driver.
