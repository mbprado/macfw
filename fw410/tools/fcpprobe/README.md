# fcpprobe

Read-only AV/C status probe for the operational M-Audio FireWire 410.

`fcpprobe` exists because the FW410 operational personality (`Unit_SW_Version = 0x14001`) does not match Apple's generic `IOFireWireAVCUnit` personality, so `IOFireWireAVCLibUnitInterface::AVCCommand()` is not available for this device.

Instead, the tool implements the minimum standard FCP transport directly with `IOFireWireLib`:

- opens the operational `FW 410` unit;
- creates a local pseudo address space for the standard FCP response register in initial-units space (`0xfffff0000d00`);
- enables write callbacks on that response window;
- sends one AV/C STATUS command to the remote FCP command register (`0xfffff0000b00`);
- waits for the matching response and prints the raw bytes;
- decodes the AM824 sample-frequency code when the response matches the expected OUTPUT PLUG SIGNAL FORMAT shape.

## Build

```bash
make
```

## Run

```bash
./fcpprobe
```

The initial query is the same read-only signal-format STATUS command used by Linux `snd-firewire` for output plug 0:

```text
01 ff 18 00 90 ff ff ff
```

Meaning:

```text
01  AV/C STATUS
ff  UNIT
18  OUTPUT PLUG SIGNAL FORMAT
00  plug 0
90  AM824 format
ff  unknown/current SFC requested
ff  SYT high unused
ff  SYT low unused
```

## Safety

This is not a configuration command. It does not change sample rate, clock source, mixer state, routing, firmware, or streaming state. It writes only the standard AV/C STATUS request to the FCP command register and receives the device's response.

No arbitrary FCP command interface is exposed yet.
