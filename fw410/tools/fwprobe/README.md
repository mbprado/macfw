# fwprobe

Minimal user-space FireWire diagnostic for M1.

The probe intentionally does **not** open the FW410 for exclusive access and does not perform arbitrary writes. It discovers FireWire services, obtains `IOFireWireDeviceInterface`, reports bus generation / node information, and can optionally inspect the device configuration ROM or perform narrowly scoped read-only BeBoB register probes.

## Build

Build on the target Intel Mac with the installed Xcode Command Line Tools:

```bash
make
```

## Basic probe

Run:

```bash
./fwprobe
```

If the FW410 is attached, the program should print the FireWire services visible to the process and the interface information it can obtain.

## Configuration ROM inspection

Run:

```bash
./fwprobe --rom
```

This mode uses:

```text
IOFireWireDeviceInterface
        |
        v
GetConfigDirectory()
        |
        v
IOFireWireConfigDirectoryInterface
        |
        +-- GetNumEntries()
        +-- GetIndexKey()
        +-- GetIndexType()
        +-- GetIndexValue_*()
        +-- recursive subdirectory traversal
```

The ROM walker reports each entry's index, key, type, and raw value. Depending on the IEEE 1394 configuration-ROM entry type, it also reports:

- immediate 32-bit values;
- offset addresses;
- textual or binary leaf data;
- recursively decoded subdirectories.

Binary leaf previews are limited to the first 32 bytes. Recursion is capped at 16 directory levels as a defensive guard.

## BeBoB software build date probe

Run:

```bash
./fwprobe --info-date
```

This performs exactly one read-only FireWire block transaction to:

```text
address: 0xffffc8020020
length:  8 bytes
```

Linux `snd-bebob` defines the BeBoB information register at `0xffffc8020000` and reads offset `0x20` before sending any M-Audio firmware-loader cue. The returned bytes are printed both as hexadecimal and ASCII. `fwprobe` deliberately does not interpret or byte-swap the value yet; the first goal is to observe the exact bytes returned by macOS and compare them with Linux and the original M-Audio driver.

The request uses the current bus generation and remote node ID, and fails on a FireWire bus reset rather than retrying with stale addressing.

### Safety

`--rom` and `--info-date` are read-only. They do **not**:

- issue FireWire writes;
- reset the FireWire bus;
- upload or start firmware;
- send the M-Audio bootloader cue;
- allocate isochronous channels;
- start audio or MIDI streaming.

## Confirmed FW410 bootloader result

On Intel macOS Monterey, `--rom` successfully decoded the FW410 bootloader unit directory:

```text
specifier:        0x00a02d
software version: 0x014001
model:            0x010058
text descriptor:  FW Bootloader
```

This result matches the Linux `snd-bebob` FW410 bootloader model. See [`../../analysis/bootloader-rom.md`](../../analysis/bootloader-rom.md) for the recorded data and Linux correlation.

## Help

```bash
./fwprobe --help
```

## Current M1 progress

1. [x] Discover `IOFireWireUnit` services.
2. [x] Obtain `IOFireWireDeviceInterface` from user space.
3. [x] Read bus generation and remote node ID.
4. [x] Add and validate read-only configuration-ROM inspection on FW410 hardware.
5. [x] Validate bootloader model/specifier identity against Linux `snd-bebob`.
6. [x] Add a narrowly scoped read-only BeBoB information-register probe (`--info-date`).
7. [ ] Validate the BeBoB information-register read on FW410 hardware.
8. [ ] Add isochronous capability probing.
9. [ ] Repeat successful probes on Intel macOS Sonoma.

Do not add arbitrary write operations to this probe until a specific FW410 register/command has been verified against the original M-Audio driver and/or another trusted implementation such as Linux `snd-bebob` or FFADO.
