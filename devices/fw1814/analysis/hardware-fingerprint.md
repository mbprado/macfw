# FW1814 hardware fingerprint

Observed on the local development M-Audio FireWire 1814 on macOS on 2026-09-06 using the read-only `fwprobe --rom --info` path before and after the guarded boot-from-flash cue.

## Bootloader personality

```text
FireWire Product Name: FW 1814 Bootloader
Vendor_ID:              0x00000d6c
GUID:                   0x000d6c0410f96d0c
Unit_Spec_ID:           0x0000a02d
Unit_SW_Version:        0x00014001
Unit directory value:   0x00010070
```

BeBoB information block at `0xffffc8020000`:

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

## Guarded boot-from-flash validation

`make -C devices/fw1814/tools boot-check` passed every FW1814-specific registry and BeBoB information-block guard.

`make -C devices/fw1814/tools boot` then sent the documented M-Audio 12-byte boot-from-flash cue to `0xffffc8021000` and the write completed successfully. The expected FireWire bus reset/re-enumeration followed.

## Operational personality

After the guarded boot cue, the same physical interface re-enumerated as:

```text
FireWire Product Name: FW 1814
Vendor_ID:              0x00000d6c
GUID:                   0x000d6c0400f96d0c
Unit_Spec_ID:           0x0000a02d
Unit_SW_Version:        0x00014001
Unit directory value:   0x00010071
```

The change from registry GUID `0x000d6c0410f96d0c` in the bootloader personality to `0x000d6c0400f96d0c` in the operational personality is observed on this unit. Neither value should be used as a model-wide identity match because GUIDs are device-specific.

The operational BeBoB information block remains:

```text
manufacturer:         bridgeCo
protocol version:     0x00000001
bootloader version:   0x00000000
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

The observed hardware model ID `0x83` and firmware/bootloader dates match the published Linux/snd-firewire-ctl-services FW1814 reference.

The configuration-ROM unit-directory value is `0x10070` in the bootloader personality and `0x10071` in the operational personality. The operational `0x10071` value also matches the Linux `MODEL_MAUDIO_FW1814` identifier.

Both locally observed personalities are now safe to represent in the experimental FW1814 device profile using product name + `Unit_Spec_ID` + `Unit_SW_Version`. The profile remains experimental and is not yet promoted into the production installer/runtime registry.

## Next step

Run the common observational BeBoB operational probe:

```bash
make -C devices/fw1814/tools operational-probe
```

This collects current AV/C signal formats, supported BridgeCo stream formations and CMP plug state. It sends AV/C STATUS/list commands only; it sends no AV/C CONTROL, mixer/configuration or streaming writes.
