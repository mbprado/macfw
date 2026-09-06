# FW1814 special-firmware notes

## Confirmed local behavior

The development FW1814 boots from `FW 1814 Bootloader` to `FW 1814` using the guarded M-Audio boot-from-flash cue.

The first operational probe established that standard AV/C PLUG SIGNAL FORMAT STATUS works, but attempting the BridgeCo extended STREAM FORMAT SUPPORT command caused the device to stop responding reliably. A subsequent FireWire write returned `0xe00002d6` (`kIOReturnTimeout`), and direct CMP reads then failed in the same session.

The BridgeCo extension command must therefore not be used on FW1814.

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

These are upstream reference formations. They have not yet been validated by macfw isochronous capture on the local FW1814.

## macfw bring-up policy

- Never send BridgeCo extended stream-format enumeration to FW1814.
- After booting operational firmware, apply the explicit FireWire bus-reset workaround before further operational transactions.
- Validate CMP and standard AV/C STATUS after the reset.
- Only then implement the known-safe M-Audio special-firmware initialization CONTROL as an explicit guarded step.
- Keep the first transport baseline at 48 kHz, internal clock, S/PDIF digital mode.
- Preserve the MIDI AM824 position but defer CoreMIDI exposure.
