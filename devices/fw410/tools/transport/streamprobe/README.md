# streamprobe

Read-only BridgeCo/BeBoB stream-topology probe for the operational M-Audio FireWire 410.

This tool builds on the raw FCP transport already validated by `fcpprobe`. It sends only AV/C STATUS requests to BridgeCo's extended PLUG INFO interface for unit isochronous plug 0 in both directions.

For each direction it queries:

- plug type (`info type 0x00`);
- channel count (`info type 0x02`);
- channel-position map (`info type 0x03`);
- section type for every returned section (`info type 0x07`).

The BridgeCo plug address used by Linux `snd-bebob` for unit isochronous plug 0 is:

```text
unit      ff
 direction 01 = output, 00 = input
mode      00 = unit
unit type 00 = isochronous
plug id   00
reserved  ff
```

The command family uses AV/C STATUS / GENERAL PLUG INFO / BridgeCo extension:

```text
01 ff 02 c0 ...
```

## Build

```bash
make
```

## Run

```bash
./streamprobe
```

## Safety

`streamprobe` performs discovery only. It does not create CMP connections, allocate isochronous bandwidth, change sample rate or clock source, start streaming, alter mixer/routing state, or write firmware.

The output is intended to map the PCM/MIDI section layout inside the FW410's AMDTP streams before any isochronous transport code is attempted.
