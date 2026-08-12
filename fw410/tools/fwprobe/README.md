# fwprobe

Minimal user-space FireWire diagnostic for M1.

The first version intentionally does **not** open the FW410 for exclusive access and does not perform arbitrary writes. It only discovers FireWire services, obtains `IOFireWireDeviceInterface`, and reports bus generation / node information.

## Build

Build on the target Intel Mac with the installed Xcode Command Line Tools:

```bash
make
```

Run:

```bash
./fwprobe
```

If the FW410 is attached, the program should print the FireWire services visible to the process and the interface information it can obtain.

## Next additions

1. Identify FW410-specific registry properties.
2. Add configuration-ROM inspection.
3. Add a safe FireWire read after the target address is confirmed from protocol analysis.
4. Add isochronous capability probing.

Do not add arbitrary write operations to this probe until a specific FW410 register/command has been verified against the original driver or another trusted implementation.
