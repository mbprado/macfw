# fwboot

Guarded one-shot boot-from-flash utility for the M-Audio FireWire 410 bootloader.

This tool is intentionally separate from `fwprobe`: `fwprobe` remains read-only, while `fwboot` contains the first write-capable operation in the project.

## Scope

`fwboot` only considers FireWire units matching the confirmed FW410 bootloader unit identity:

```text
Unit_Spec_ID:    0x0000a02d
Unit_SW_Version: 0x00014001
```

Before any write, it opens the device and reads the BeBoB information block at `0xffffc8020000`. It refuses to proceed unless all of the following are true:

```text
protocol version:   1
bootloader version: non-zero
software date:      >= 20070401
software ID:        0x00010046
```

These conditions match the Linux `snd-bebob` FW410 boot-from-flash path and the values confirmed on the tested hardware.

## Build

```bash
make
```

## Dry run / preflight

```bash
./fwboot
```

This performs no write. It only validates the device state.

## Execute the boot-from-flash cue

```bash
./fwboot --execute
```

After the preflight passes, the tool performs exactly one 12-byte FireWire block write to:

```text
0xffffc8021000
```

with the same logical cue used by Linux `snd-bebob`:

```text
0x00000001
0x01110000
0x00000000
```

which is sent as little-endian wire bytes:

```text
01 00 00 00 00 00 11 01 00 00 00 00
```

A successful cue is expected to make the FW410 load its application from flash and reset/re-enumerate on the FireWire bus. The old device interface and bus generation may therefore become stale immediately after the write.

After execution, wait for re-enumeration and run:

```bash
../fwprobe/fwprobe --rom
```

The expected model transition is:

```text
bootloader: 0x00010058
normal FW410: 0x00010046
```

## Safety notes

This is not a firmware uploader and does not write flash contents. It only sends the documented boot-from-flash cue after strict live-device checks. Do not generalize this utility into an arbitrary FireWire write tool without a separate design/review step.
