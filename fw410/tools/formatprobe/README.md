# formatprobe

Read-only BridgeCo/BeBoB stream-format list probe for the operational M-Audio FireWire 410.

Linux `snd-bebob` enumerates stream formations with the BridgeCo `STREAM FORMAT SUPPORT` extension (`opcode 0x2f`, list request `0xc1`). `formatprobe` sends those same STATUS requests for unit isochronous plug 0 in both directions.

For each direction it starts at entry ID 0 and continues until the device reports the entry unavailable or the conservative local limit is reached.

Example request shape:

```text
01 ff 2f c1 DIR 00 00 00 ff 00 EID 00
```

where `DIR` is device-relative: `01` is FW410 OUTPUT (host capture/input), while `00` is FW410 INPUT (host playback/output).

## Decoded mode

Default output decodes each supported formation into sample rate, PCM channel count, MIDI port count, and cluster count.

The observed FW410 BridgeCo payload shape is:

```text
90 40 FREQ 01 CLUSTERS [CHANNELS FORMAT]...
```

For this device:

- cluster format `0x06` is MBLA/PCM audio;
- cluster format `0x0d` is MIDI-conformant data;
- the BridgeCo frequency codes map through the same table used by Linux `snd-bebob`.

Run:

```bash
./formatprobe
```

## Raw mode

Use `--raw` to keep the decoded summary while also printing every AV/C command, response, format payload, and decoded cluster:

```bash
./formatprobe --raw
```

This is useful when comparing results with Linux/FFADO or when investigating another BeBoB device.

## Build

From this directory:

```bash
make clean
make
```

Or build every FW410 tool from `fw410/tools`:

```bash
make
```

and clean every tool with:

```bash
make clean
```

## Safety

All requests are AV/C STATUS/list queries. The tool does not change sample rate, establish CMP connections, allocate isochronous resources, or start audio streaming.
