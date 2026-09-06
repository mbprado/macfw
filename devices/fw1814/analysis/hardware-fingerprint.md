# FW1814 hardware fingerprint

Observed on the local development M-Audio FireWire 1814 on macOS on 2026-09-06 using the read-only `fwprobe --rom --info` path.

## Status

**Observed personality:** bootloader

No control or streaming writes were issued while collecting this fingerprint.

## IORegistry / configuration-ROM identity

```text
FireWire Product Name: FW 1814 Bootloader
Vendor_ID:              0x00000d6c
GUID:                   0x000d6c0410f96d0c
Unit_Spec_ID:           0x0000a02d
Unit_SW_Version:        0x00014001
Unit directory value:   0x00010070
```

The GUID is unique to the development unit and must not be used as a model-wide identity match.

## BeBoB information block

Read from `0xffffc8020000`:

```text
manufacturer:         bridgeCo
protocol version:     0x00000001
bootloader version:   0x00002805
GUID (FFADO):         0x000d6c0400f96d0c
hardware model ID:    0x00000083
hardware revision:    0x00000001
software date:        20070713
software time:        080440
software ID:          0x00000000
software version:     0x00000000
base address:         0x20080000
max image length:     0x00180000
bootloader date:      20040330
bootloader time:      025909
```

## Interpretation

The observed hardware model ID (`0x83`) and firmware/bootloader dates match the published Linux/snd-firewire-ctl-services reference for the FireWire 1814. This is therefore a strong model-level correlation in addition to the local macOS observation.

The operational FireWire personality remains unconfirmed locally. It must be fingerprinted after a guarded boot-from-flash operation before it is added to production identity matching.

## Boot-from-flash next step

`devices/fw1814/tools/fwboot1814` implements the documented M-Audio boot-from-flash cue with FW1814-specific guards. It requires the confirmed registry identity plus the observed BeBoB model/firmware fingerprint before a write can occur.

The dry-run path is:

```bash
make -C devices/fw1814/tools boot-check
```

The guarded write path is:

```bash
make -C devices/fw1814/tools boot
```

A FireWire bus reset/re-enumeration is expected after the cue. The next action is to rerun:

```bash
make -C devices/fw1814/tools fingerprint
```

and record the operational personality.
