# FW410 isochronous transport bring-up

This document records the first confirmed user-space IEEE 1394 isochronous transport results for the M-Audio FireWire 410 on Intel macOS Monterey.

## Milestones reached

The following path is now experimentally confirmed:

1. Bootloader is detected as `FW Bootloader`.
2. `fwboot --execute` sends the guarded boot-from-flash cue.
3. The device re-enumerates as operational `FW 410`.
4. Raw FCP/AV/C command and response transport works from user space.
5. BridgeCo stream topology and supported formations are discovered.
6. FireWire IRM channel/bandwidth allocation works from user space.
7. Both CMP directions can be established simultaneously and restored exactly.
8. A local NuDCL receive program receives FW410 isochronous packets.
9. With only CMP established, the FW410 transmits AM824 NODATA packets.
10. When the Mac also transmits a valid isochronous AM824 NODATA stream to the FW410, the FW410 immediately transitions to data-bearing capture packets.
11. Capture MBLA words are decoded as signed 24-bit PCM and accumulated per channel.
12. A controlled signal on the front Analog Input 1 is confirmed on the expected PCM stream position.

The duplex result confirms the FireWire 410 behavior observed by Linux `snd-bebob`: actual packet flow is needed in both directions, not merely two established CMP connections.

## Confirmed 48 kHz transport

### Host capture / FW410 device OUTPUT

Allocated FireWire channel in the successful tests: `0`.

Data-bearing packets are 168 bytes:

```text
8-byte CIP header
+ 8 events * 5 data positions * 4 bytes
= 168 bytes
```

Observed data-bearing CIP example:

```text
00 05 00 f0 90 02 aa b5
```

Decoded:

- DBS: `5`
- DBC: `0xf0`
- FMT: `0x10` (AM824)
- FDF: `0x02` (48 kHz)
- SYT: valid (`0xaab5` in this packet)
- events in packet: `(168 - 8) / (5 * 4) = 8`

The stream uses the previously discovered position order:

```text
position 1  S/PDIF L
position 2  Analog Input 1
position 3  S/PDIF R
position 4  Analog Input 2
position 5  MIDI
```

The first four positions contain AM824 MBLA words with label byte `0x40`. The fifth position contains MIDI-conformant data; `0x80000000` is observed when no MIDI byte is present.

Example first event:

```text
0x40000000  S/PDIF L
0x40ffffbd  Analog Input 1
0x40000000  S/PDIF R
0x40ffff81  Analog Input 2
0x80000000  MIDI no-data
```

For MBLA audio the low 24 bits are the signed PCM sample payload. Thus `0x40ffffbd` represents signed 24-bit sample `-67`, and `0x40ffff81` represents `-127`.

## Physical input validation

A controlled signal was connected to the **front Analog Input 1** while `isoduplex --execute` captured 256 FireWire packet slots.

The run produced:

```text
PCM capture statistics:
    decoded events: 1536
    malformed packets: 0
    ch 1 (S/PDIF L): samples=1536 min=0 max=0 peak=0 rms=0
    ch 2 (Analog Input 1): samples=1536 min=-1697036 max=1527585 peak=1697036 rms=717516
    ch 3 (S/PDIF R): samples=1536 min=0 max=0 peak=0 rms=0
    ch 4 (Analog Input 2): samples=1536 min=-259 max=264 peak=264 rms=73.4459
```

This is a conclusive physical-to-stream mapping result:

- front Analog Input 1 -> PCM channel 2 / stream position 2
- Analog Input 2 remained at a very small noise floor
- both S/PDIF capture positions were exactly zero in this test
- all 1536 expected sample events were decoded with zero malformed packets

The front Mic/Inst connector and rear Line Input 1 are alternate physical sources for the same logical Analog Input 1 path depending on the FW410 hardware selector. For driver-facing naming, `Analog Input 1` is therefore preferable to naming the stream position after only one physical connector.

### Host playback / FW410 device INPUT

Allocated FireWire channel in the successful test: `1`.

The first duplex test deliberately transmitted only 8-byte AM824 NODATA packets:

```text
00 0b 00 00 90 02 ff ff
```

This was sufficient to cause the FW410 to begin sending real capture samples. No playback PCM samples have yet been transmitted.

The maximum 48 kHz blocking-mode playback packet is:

```text
8-byte CIP header
+ 8 events * 11 data positions * 4 bytes
= 360 bytes
```

## NODATA behavior

Before host-to-device packet transmission was started, the FW410 emitted only 8-byte CIP packets such as:

```text
00 04 00 00 90 02 ff ff
```

No sample payload follows the CIP header and SYT is `0xffff`.

Once duplex packet flow was active, the capture pattern changed to regular 168-byte data-bearing packets with periodic 8-byte NODATA packets. In the successful 256-slot observation window:

- touched receive slots: `256 / 256`
- data-bearing packets: `192`
- NODATA packets: `64`

This is exactly a 3:1 data/NODATA ratio. At 48 kHz, three packets containing 8 sample events every four 125-us FireWire cycles gives:

```text
3 * 8 events / (4 * 125 us) = 48,000 events/sec
```

So the observed FW410 capture stream is using the expected 48 kHz blocking-mode AMDTP cadence.

## DBC behavior

Data-bearing packets advance DBC by eight events:

```text
... f0, f8, 00, 08, 10, 18, 20, 28 ...
```

The periodic NODATA packet retains the next expected DBC value and does not consume data blocks. This agrees with the blocking-mode interpretation.

## Cleanup behavior

All current transport experiments save the exact original oPCR0/iPCR0 values and restore them on exit. Successful duplex tests end with:

```text
restore iPCR[0]: success
restore oPCR[0]: success
post-test PCR restore: PASS
```

This property should be preserved as the streaming engine evolves.

## Next steps

1. Implement correctly timed data-bearing host playback packets, initially digital silence.
2. Validate playback DBC/SYT generation against the FireWire cycle clock.
3. Send a low-level test tone to one playback channel and verify the corresponding physical FW410 output.
4. Validate long-running duplex continuity and recovery across bus resets.
5. Refactor the experimental transport into reusable library classes.
6. Build the CoreAudio-facing user-space device layer on top of the validated transport.
