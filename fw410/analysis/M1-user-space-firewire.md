# M1 — User-space FireWire proof of concept

## Objective

Establish whether an ordinary user-space process on an Intel Mac running macOS Sonoma can access the FireWire bus and communicate with the FW410 **without loading the original M-Audio kext**.

This milestone deliberately comes before AudioDriverKit work.

## Current status

The first hardware proof has been completed on an Intel Mac running macOS Monterey. A normal user-space process can discover the FW410 bootloader, obtain `IOFireWireDeviceInterface` version 8, read the FireWire bus generation and node ID, and inspect the remote configuration ROM through `IOFireWireConfigDirectoryInterface`.

This is a strong validation of the proposed user-space transport architecture, but it is **not yet proof for Sonoma**. Sonoma remains the final M1 compatibility target.

The observed bootloader ROM and Linux correlation are documented in [`bootloader-rom.md`](bootloader-rom.md).

## Hypothesis

```text
User-space diagnostic
        │
        ▼
IOFireWireLib
        │
        ▼
IOFireWireDeviceInterface
        │
        ├── device discovery
        ├── bus generation
        ├── node IDs
        ├── configuration ROM
        ├── async read/write
        └── isochronous interfaces
        │
        ▼
Apple FireWire stack
        │
        ▼
FW410
```

If the same path remains operational on Sonoma, it gives us a much simpler path to the DevKit:

```text
CoreAudio / AudioDriverKit
             │
             ▼
       macfw Audio API
             │
             ▼
     macfw FireWire API
             │
             ▼
         IOFireWireLib
             │
             ▼
            FW410
```

If it does not work on Sonoma, we then investigate a DriverKit-based FireWire transport implementation.

## M1 tests

### Test 1 — FireWire registry discovery

Find `IOFireWireController`, `IOFireWireDevice`, and `IOFireWireUnit` objects in the I/O Registry.

Record:

- vendor ID
- GUID
- product name
- unit specification ID
- software version
- node information
- controller properties

**Monterey:** passed for the FW410 bootloader.

### Test 2 — User-space interface acquisition

For an FW410-related FireWire service:

1. Create an `IOCFPlugInInterface` with `IOCreatePlugInInterfaceForService`.
2. Request `kIOFireWireDeviceInterfaceID`.
3. Record the interface revision/version.
4. Query the bus generation and remote node ID.

**Monterey:** passed. `IOFireWireDeviceInterface` version 8 was acquired successfully.

### Test 3 — Configuration ROM

Use the FireWire library configuration-directory interface to inspect the FW410's ROM and identify:

- vendor/model identity
- unit directories
- firmware/software information
- textual descriptors

**Monterey:** passed. The bootloader unit directory reports specifier `0x00a02d`, software version `0x014001`, model `0x010058`, and descriptor `FW Bootloader`.

### Test 4 — Harmless asynchronous access

After the previous tests succeed, perform a read from a known-safe FireWire address used by the FW410 protocol.

Linux `snd-bebob` gives us a strong candidate: an 8-byte read at absolute FireWire address `0xffffc8020020`, used to read the M-Audio bootloader firmware-build date before any write is attempted.

Do **not** perform arbitrary writes during this probe.

### Test 5 — Isochronous capability

Determine whether the user-space library can create and operate the required isochronous channel/port objects and whether callbacks work from a normal run loop.

## Success criteria

### Proven on Monterey

- [x] FW410 is visible in the I/O Registry.
- [x] A user-space process obtains an `IOFireWireDeviceInterface`.
- [x] Bus generation can be read.
- [x] FW410 node ID can be read.
- [x] Configuration ROM can be inspected.
- [ ] At least one safe asynchronous transaction can be completed.
- [ ] Isochronous API availability is established by an actual runtime test.

### Still required for final M1

- [ ] Repeat the successful user-space tests on Intel macOS Sonoma.

The asynchronous and isochronous tests determine whether the first DevKit transport can wrap Apple's existing FireWire user-space API or whether we need to implement a new FireWire transport.

## Decision point

```text
              M1 probe
                 │
       ┌─────────┴─────────┐
       │                   │
       ▼                   ▼
   IOFireWireLib       insufficient
     works               access
       │                   │
       ▼                   ▼
 wrap existing        investigate
 user-space API       DriverKit FW
       │              transport
       └─────────┬─────────┘
                 ▼
           macfw FireWire
              DevKit
```
