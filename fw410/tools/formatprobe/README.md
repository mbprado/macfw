# formatprobe

Read-only BridgeCo/BeBoB stream-format list probe for the operational M-Audio FireWire 410.

Linux `snd-bebob` enumerates stream formations with the BridgeCo `STREAM FORMAT SUPPORT` extension (`opcode 0x2f`, list request `0xc1`). `formatprobe` sends those same STATUS requests for unit isochronous plug 0 in both directions.

For each direction it starts at entry ID 0 and continues until the device reports the entry unavailable or the conservative local limit is reached.

Example request shape:

```text
01 ff 2f c1 DIR 00 00 00 ff 00 EID 00
```

where `DIR` is `01` for OUTPUT and `00` for INPUT.

The first version intentionally prints the returned format payload raw. We will decode it only after observing the FW410's actual entries and correlating them with Linux's formation parser.

## Build and run

```bash
make clean
make
./formatprobe
```

## Safety

All requests are AV/C STATUS/list queries. The tool does not change sample rate, establish CMP connections, allocate isochronous resources, or start audio streaming.
