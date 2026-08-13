# cmpprobe

Read-only IEC 61883 Connection Management Procedures (CMP) register probe for the operational M-Audio FireWire 410.

It reads the standard CSR registers used by Linux `snd-bebob` before establishing isochronous connections:

- `oMPR` at `0xfffff0000900`
- `oPCR[0]` at `0xfffff0000904`
- `iMPR` at `0xfffff0000980`
- `iPCR[0]` at `0xfffff0000984`

The tool decodes plug count, online state, broadcast/point-to-point connection state, current isochronous channel, and output speed fields. It does not allocate bandwidth, modify PCR values, establish CMP connections, or start streaming.

Direction labels are device-relative:

- `oPCR[0]`: FW410 OUTPUT -> host capture/input
- `iPCR[0]`: FW410 INPUT <- host playback/output

## Build and run

From `fw410/tools`:

```bash
make cmpprobe
./cmpprobe/cmpprobe
```

Or build all tools with:

```bash
make
```
