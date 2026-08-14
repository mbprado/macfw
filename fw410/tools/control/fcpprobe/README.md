# fcpprobe

Read-only AV/C/FCP discovery probe for the operational M-Audio FireWire 410.

`fcpprobe` exists because the FW410 operational personality (`Unit_SW_Version = 0x14001`) does not match Apple's generic `IOFireWireAVCUnit` personality, so `IOFireWireAVCLibUnitInterface::AVCCommand()` is not available for this device.

Instead, the tool implements the minimum standard FCP transport directly with `IOFireWireLib`:

- opens the operational `FW 410` unit;
- creates a local pseudo address space at the standard FCP response CSR (`0xfffff0000d00`);
- enables write callbacks on that response window;
- sends AV/C STATUS commands to the remote FCP command CSR (`0xfffff0000b00`);
- receives and prints the raw FCP response frames;
- decodes current AM824 sample-frequency codes for output plug 0 and input plug 0;
- checks whether the two directions agree on sample rate;
- queries AV/C unit `PLUG INFO` subfunction 0 and prints the four returned plug-count operands.

## Build

```bash
make
```

## Run

```bash
./fcpprobe
```

The probe currently sends these read-only AV/C STATUS commands:

```text
01 ff 18 00 90 ff ff ff   OUTPUT PLUG SIGNAL FORMAT, plug 0
01 ff 19 00 90 ff ff ff   INPUT PLUG SIGNAL FORMAT, plug 0
01 ff 02 00 00 00 00 00   UNIT PLUG INFO, subfunction 0
```

## Confirmed hardware result

On the tested Intel Mac / macOS Monterey system, the operational FW410 already responded successfully through the raw user-space FCP transport to the output-plug query:

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

Linux `snd-bebob` queries both output and input plug 0 and expects their current rates to agree before stream setup. The extended probe now performs that same read-only comparison.

The `PLUG INFO` response gives AV/C unit plug-count fields. It is intentionally not treated as PCM/MIDI channel topology; the channel layout inside the AMDTP stream requires BridgeCo extended stream-format and channel-position discovery.

## Safety

All commands currently exposed by this tool are AV/C STATUS queries. They do not change sample rate, clock source, mixer state, routing, firmware, CMP connections, or streaming state.

No arbitrary FCP command interface is exposed yet.
