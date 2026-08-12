# fwprobe

Minimal user-space FireWire diagnostic for M1.

The probe intentionally does **not** open the FW410 for exclusive access and does not perform arbitrary writes. It discovers FireWire services, obtains `IOFireWireDeviceInterface`, reports bus generation / node information, and can optionally inspect the device configuration ROM through Apple's read-only `IOFireWireConfigDirectoryInterface` API.

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

### Safety

`--rom` is read-only. It does **not**:

- issue FireWire writes;
- reset the FireWire bus;
- upload or start firmware;
- allocate isochronous channels;
- start audio or MIDI streaming.

## Help

```bash
./fwprobe --help
```

## Current M1 progress

1. [x] Discover `IOFireWireUnit` services.
2. [x] Obtain `IOFireWireDeviceInterface` from user space.
3. [x] Read bus generation and remote node ID.
4. [x] Add read-only configuration-ROM inspection.
5. [ ] Validate the FW410 bootloader ROM output against Linux/ALSA and FFADO.
6. [ ] Add a safe FireWire read after the target address is confirmed from protocol analysis.
7. [ ] Add isochronous capability probing.

Do not add arbitrary write operations to this probe until a specific FW410 register/command has been verified against the original M-Audio driver and/or another trusted implementation such as Linux `snd-bebob` or FFADO.
