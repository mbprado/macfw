# FW1814 first successful duplex capture

Date: 2026-09-06

This records the first macfw run that successfully started the M-Audio FireWire 1814 special-firmware transport and received device-to-host AMDTP capture packets on the local hardware.

## Preconditions

The unit was operational at 48 kHz with the guarded special-firmware baseline:

- internal clock;
- S/PDIF digital input/output mode;
- unlocked clock control;
- authoritative INPUT PLUG SIGNAL FORMAT STATUS = 48000 Hz.

The transport used the BeBoB blocking model discovered from upstream Linux:

- host -> device playback formation: 6 PCM + 1 MIDI, DBS=7;
- device -> host capture formation: 10 PCM + 1 MIDI, DBS=11;
- playback max packet reservation: 232 bytes;
- capture max packet reservation: 360 bytes;
- playback starts before capture;
- both CMP directions are connected before the M-Audio special rate kick;
- idle playback sends timed AM824 silence rather than permanent NODATA.

## Successful special-firmware rate kick

The successful run was `duplex-blocking-raw`.

Observed FCP exchanges:

```text
OUTPUT CONTROL
command:  00 ff 18 00 90 02 ff ff
response: 09 ff 18 00 90 02 ff ff
result: PASS

wait 100 ms

INPUT CONTROL
command:  00 ff 19 00 90 02 ff ff
response: 0f ff 19 00 90 02 ff ff
result: PASS
```

An immediate STATUS readback was intentionally skipped during startup, matching the Linux special-firmware sequence.

## Capture result

The receive ring completed with:

```text
touched slots: 64 / 64
```

Data-bearing packets were 360 bytes and had the expected capture CIP formation:

```text
DBS = 11
FMT = 0x10
FDF = 0x02
```

DBC advanced by 8 across each data-bearing packet. Example sequence from the capture:

```text
data   dbc=136 len=360
data   dbc=144 len=360
NODATA dbc=152 len=8
data   dbc=152 len=360
data   dbc=160 len=360
data   dbc=168 len=360
NODATA dbc=176 len=8
data   dbc=176 len=360
```

This confirms blocking-mode behavior: NODATA cycles do not consume data blocks, so DBC is held across the NODATA packet and advances only with the following eight-event data packet.

### Important NODATA observation

The device's eight-byte NODATA packets were observed as:

```text
01 02 00 <dbc> 90 02 ff ff
```

Therefore the NODATA packet's CIP DBS field is `2`, not the data-bearing capture DBS value `11`. Validators must classify these packets by their NODATA semantics (`length == 8`, `SYT == 0xffff`) and must not require `DBS == 11` for NODATA packets.

## AM824 position observations

Each data-bearing packet carries eight events × eleven quadlets/event.

In the captured S/PDIF-baseline stream, positions 0 through 7 contained valid MBLA-labelled (`0x40`) sample words. Positions 8 and 9 were observed as zero quadlets in this run, and position 10 carried the MIDI no-data word `0x80000000`.

This is wire evidence only. Physical input names/order are intentionally not assigned yet. The next mapping phase must inject known signals into one physical FW1814 input at a time and measure activity per AM824 position.

## Post-test state

Both PCRs restored exactly and INPUT PLUG SIGNAL FORMAT STATUS read back 48000 Hz after disconnect.

The interface remained operational.

A later process observed a bus-generation increment after the streaming process had exited. Separate observer diagnostics established that the generation remained stable during active transport and through all explicitly traced ISO/CMP teardown steps. That late generation increment therefore does not explain the in-process startup result and should be investigated independently from AMDTP formation.

## Timing caveat

A previous non-raw run of the same blocking transport failed the INPUT CONTROL step, while this raw run succeeded. The command bytes and nominal 100 ms delay were unchanged. Raw logging adds a small amount of execution latency around the FCP transaction path, so startup timing remains a candidate variable.

Do not yet treat the 100 ms macfw sequence as deterministic. Before productionizing the transport, measure/parameterize the OUTPUT-to-INPUT timing and establish a repeatable non-raw startup.

## Proven milestone

This run proves that macfw can start the FW1814 special-firmware duplex transport and receive real 48 kHz device-to-host AMDTP packets without using any unsupported BridgeCo stream-format extension commands.
