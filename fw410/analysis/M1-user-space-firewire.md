# M1 — User-space FireWire proof of concept

## Objective

Establish whether an ordinary user-space process on an Intel Mac running macOS Sonoma can access the FireWire bus and communicate with the FW410 **without loading the original M-Audio kext**.

This milestone deliberately comes before AudioDriverKit work.

## Important finding

Apple's current IOKit documentation still exposes `IOFireWireLib` device interfaces, including `IOFireWireDeviceInterface`, asynchronous command interfaces, isochronous channel/port interfaces, configuration-ROM access, and synchronous read/write operations. This means the first hypothesis to test is **not** that we need to immediately rebuild the OHCI/FireWire stack. We should first determine how much of the existing user-space FireWire API remains operational on the target Sonoma installation.

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

If this works, it gives us a much simpler path to the DevKit:

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

If it does not work, we then investigate a DriverKit-based FireWire transport implementation.

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

### Test 2 — User-space interface acquisition

For an FW410-related FireWire service:

1. Create an `IOCFPlugInInterface` with `IOCreatePlugInInterfaceForService`.
2. Request `kIOFireWireDeviceInterfaceID`.
3. Record the interface revision/version.
4. Query the bus generation and remote node ID.

### Test 3 — Configuration ROM

Use the FireWire library configuration-directory interface to inspect the FW410's ROM and identify:

- vendor
- model
- unit directories
- firmware/software information
- AV/C unit information

### Test 4 — Harmless asynchronous access

After the previous tests succeed, perform a read from a known-safe FireWire address used by the FW410 protocol.

Do **not** perform arbitrary writes during the initial probe.

### Test 5 — Isochronous capability

Determine whether the user-space library can create and operate the required isochronous channel/port objects and whether callbacks work from a normal run loop.

## Success criteria

M1 is successful if we can demonstrate, without the M-Audio kext:

- [ ] FW410 is visible in the I/O Registry.
- [ ] A user-space process obtains an `IOFireWireDeviceInterface`.
- [ ] Bus generation can be read.
- [ ] FW410 node ID can be read.
- [ ] Configuration ROM can be inspected.
- [ ] At least one safe asynchronous transaction can be completed.
- [ ] Isochronous API availability is established.

The last two items determine whether the first DevKit transport can wrap Apple's existing FireWire user-space API or whether we need to implement a new FireWire transport.

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
