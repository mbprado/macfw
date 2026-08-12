# fcpprobe

Read-only AV/C status probe for the operational M-Audio FireWire 410.

`fcpprobe` exists because the FW410 operational personality (`Unit_SW_Version = 0x14001`) does not match Apple's generic `IOFireWireAVCUnit` personality, so `IOFireWireAVCLibUnitInterface::AVCCommand()` is not available for this device.

Instead, the tool implements the minimum standard FCP transport directly with `IOFireWireLib`:

- opens the operational `FW 410` unit;
- creates a local pseudo address space for the standard FCP response register in initial-units space (`0xfffff0000d00`);
- enables write callbacks on that response window;
- sends an AV/C STATUS command to the remote FCP command register (`0xfffff0000b00`);
- waits for the response and prints the raw bytes;
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

## Confirmed hardware result

On the tested Intel Mac / macOS Monterey system, the operational FW410 responded successfully through the raw user-space FCP transport:

```text
command:  01 ff 18 00 90 ff ff ff
response: 0c ff 18 00 90 02 ff ff
```

Interpretation:

```text
0c  IMPLEMENTED/STABLE
ff  UNIT
18  OUTPUT PLUG SIGNAL FORMAT
00  plug 0
90  AM824
02  sample-frequency code 2 = 48000 Hz
```

This confirms both directions of the FCP transaction path from user space:

```text
macfw -> remote FCP command CSR -> FW410
macfw <- local FCP response CSR  <- FW410
```

The first successful transaction reported the current output plug 0 sample rate as **48 kHz**.

## Next discovery step

Linux `snd-bebob` queries both output plug 0 and input plug 0 and expects the two rates to agree before streaming. The next read-only probe should therefore send:

```text
01 ff 19 00 90 ff ff ff
```

for `INPUT PLUG SIGNAL FORMAT`, followed by AV/C `PLUG INFO` discovery of the unit's streaming plug counts.

## Safety

These are STATUS queries. They do not change sample rate, clock source, mixer state, routing, firmware, or streaming state.

No arbitrary FCP command interface is exposed yet.
