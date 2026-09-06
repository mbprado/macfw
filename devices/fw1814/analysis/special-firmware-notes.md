# FW1814 special-firmware notes

## Confirmed local behavior

The development FW1814 boots from `FW 1814 Bootloader` to `FW 1814` using the guarded M-Audio boot-from-flash cue.

The first operational probe established that standard AV/C PLUG SIGNAL FORMAT STATUS works, but attempting the BridgeCo extended STREAM FORMAT SUPPORT command caused the device to stop responding reliably. A subsequent FireWire write returned `0xe00002d6` (`kIOReturnTimeout`), and direct CMP reads then failed in the same session.

The BridgeCo extension command must therefore not be used on FW1814.

A dedicated post-boot FireWire bus reset was then tested on the local unit. The first helper revision incorrectly called `BusReset()` before `Open()` and returned `0xe00002cd` (`kIOReturnNotOpen`). After fixing the IOFireWireLib lifecycle to `Open() -> BusReset() -> Close()`, the reset completed successfully with `0x0` and advanced the observed bus generation from 10 to 11.

Immediately after the successful reset, the corrected special-firmware probe produced a clean CMP snapshot and a successful standard AV/C INPUT PLUG SIGNAL FORMAT STATUS read:

```text
oMPR:    0xbfff0002  plugs=2
oPCR[0]: 0x80000080  online=yes p2p=0 broadcast=no channel=0
iMPR:    0x80ff0003  plugs=3
iPCR[0]: 0x80000000  online=yes p2p=0 broadcast=no channel=0
current INPUT signal format: 44100 Hz
```

This locally confirms that the extra post-boot bus reset restores reliable normal transaction handling on the development FW1814.

The guarded special-firmware initializer was then hardware-tested at 48 kHz. The known internal-clock/S/PDIF baseline was accepted, OUTPUT 48 kHz succeeded, INPUT 48 kHz succeeded after the required 100 ms delay, and authoritative INPUT STATUS read back 48000 Hz.

The first receive-only AMDTP experiment established oPCR[0] successfully and started the host receive channel, but received zero packets (`0 / 64` NuDCL slots touched). oPCR[0] restored exactly after the test. This showed that an OUTPUT CMP connection alone does not cause this firmware to begin transmitting.

A second receive-only experiment added the M-Audio special-firmware stream kick after oPCR[0] connection and host RX startup. In that state:

```text
reassert OUTPUT 48000 Hz: PASS
wait 100 ms
reassert INPUT 48000 Hz:  FAIL
```

The same INPUT command succeeds outside streaming in `fw1814init`, so the command bytes and 100 ms timing are already hardware-validated. The failure is specific to the incomplete stream-start state.

Re-reading the current Linux BeBoB streaming engine clarified the missing condition: `snd_bebob_stream_start_duplex()` starts the host-to-device and device-to-host streams, with both CMP connections established, before the M-Audio special-firmware rate reassert is issued. Therefore the earlier macfw assumption that FW1814 startup could be modeled as an oPCR-only receive stream was incorrect.

The next incremental experiment reserved a host-to-device companion ISO channel and connected iPCR[0] as well as oPCR[0], while still starting only the capture DMA. One run accepted both OUTPUT and INPUT rate CONTROLs but an immediate STATUS readback failed; this readback is not part of Linux `special_set_rate()` and was therefore removed from the startup success criterion.

A subsequent clean run reproduced the full operational setup from bus reset through `init-48`, then ran the dual-CMP/no-readback diagnostic. Both PCRs connected and host RX started, but with companion TX DMA intentionally not running the result was:

```text
reassert OUTPUT 48000 Hz: PASS
wait 100 ms
reassert INPUT 48000 Hz:  FAIL
```

Both PCRs restored exactly afterward. This demonstrates that merely reserving/connecting both CMP directions is not a reliable substitute for actually starting the host-to-device ISO direction. The next experiment therefore runs a real looping NODATA host-to-device AMDTP stream before starting capture and issuing the special rate kick.

## Upstream Linux correlation

The Linux `snd-bebob` implementation treats FW1814 and ProjectMix as M-Audio **special firmware** devices rather than normal BridgeCo-extension devices.

Important rules from that implementation:

1. Special models do not support BridgeCo extensions.
2. The current sampling rate is read with the standard AV/C INPUT PLUG SIGNAL FORMAT status operation.
3. Digital input/output format and clock-source state are maintained in software because the driver is not allowed to query these parameters safely.
4. During discovery Linux explicitly initializes the device with the documented M-Audio vendor-dependent CONTROL command to:
   - clock source `0x03` = internal;
   - digital input format `0x00` = S/PDIF;
   - digital output format `0x00` = S/PDIF;
   - clock lock `0x00` = unlocked.
5. Linux uses static stream-formation tables derived from the selected digital modes rather than querying BridgeCo format support.
6. Linux schedules a FireWire bus reset for FW1814/ProjectMix after registration because these devices can otherwise have a FireWire gap-count mismatch that causes frequent transaction failures.
7. When changing sample rate, Linux programs device OUTPUT first, waits 100 ms, then programs device INPUT because the second command is otherwise prone to failure.
8. For M-Audio special firmware, after the CMP connections and AMDTP domain are started, Linux re-applies the current sample rate. Its source comment states that the customized firmware uses these commands to start transmitting the stream.
9. The Linux BeBoB streaming engine reserves and starts both stream directions as one duplex domain before the M-Audio special rate reassert. This is distinct from whether an application is actively using both PCM directions; transport startup itself is duplex.

## Reference 44.1/48 kHz formations

For the S/PDIF baseline used during Linux discovery:

```text
device -> host / capture:   10 PCM + 1 MIDI
host -> device / playback:   6 PCM + 1 MIDI
```

For ADAT mode at 44.1/48 kHz:

```text
device -> host / capture:   16 PCM + 1 MIDI
host -> device / playback:  12 PCM + 1 MIDI
```

These are upstream reference formations. They have not yet been validated from received macfw isochronous packets on the local FW1814.

At 48 kHz in the S/PDIF baseline, the corresponding maximum packet reservations used by the current bring-up probes are:

```text
device -> host:  8 + 6 * 11 * 4 = 272 bytes
host -> device:  8 + 6 *  7 * 4 = 176 bytes
```

The NODATA companion itself transmits only the 8-byte CIP header on each scheduled cycle, with `DBS=7`, `FDF=0x02`, and `SYT=0xffff`. The larger 176-byte allocation only reserves the bandwidth required by the configured 6-PCM-plus-MIDI playback formation.

## macfw bring-up policy

- Never send BridgeCo extended stream-format enumeration to FW1814.
- After booting operational firmware, apply the explicit FireWire bus-reset workaround before further operational transactions.
- Validate CMP and standard AV/C STATUS after the reset.
- Establish the known-safe M-Audio baseline explicitly: internal clock + S/PDIF input/output + unlocked clock controls.
- For ordinary sample-rate changes, program OUTPUT, wait 100 ms, then program INPUT and verify with INPUT PLUG SIGNAL FORMAT STATUS.
- Model FW1814 stream startup as a duplex transport handshake: both CMP directions and both host ISO directions must be accounted for before the M-Audio special-firmware rate reassert.
- Keep application-level capture/playback concerns separate from the transport startup requirement; a capture-only client can still require a silent/no-data companion transport stream underneath.
- Start host-to-device playback transport before device-to-host capture transport, matching the proven BeBoB/macfw duplex ordering.
- During bring-up, use the known-safe NODATA companion (`DBS=7`, `FDF=0x02`) rather than sample-bearing playback until device transmission is proven.
- Always restore both PCRs exactly after experiments that connect them.
- Keep the first transport baseline at 48 kHz, internal clock, S/PDIF digital mode.
- Preserve the MIDI AM824 position but defer CoreMIDI exposure.
