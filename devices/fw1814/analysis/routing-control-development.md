# FW1814 routing-control development

## Backend rule

The active transport remains the sole owner of the FireWire device handle. `fw1814ctl` sends commands to `/tmp/macfw-fw1814-control.sock`; it never opens the interface or writes FireWire directly.

FW1814 special-firmware mixer registers are write-only. The backend therefore:

1. starts from the exact routing values already written by the analog engine;
2. derives each differential update from an authoritative software cache;
3. checks that the FireWire bus generation still matches before and after the write;
4. updates the cache only after a clean write;
5. rejects every register and bitfield that is not explicitly enabled.

## Upstream register map

FFADO documents the special-firmware control area and bit layouts in [`special_avdevice.h`](https://github.com/llekn/ffado/blob/8ba6d6415f48ccb740cf4685299ef415286f4a6e/src/bebob/maudio/special_avdevice.h). Its mixer implementation also maintains a software register cache because hardware readback is unavailable.

The first writable macfw subset is only `MIX_STM_IN` at offset `0x94`:

| Bit | Route |
|---:|---|
| 3 | Software return 1/2 -> Mixer 3/4 |
| 2 | Software return 1/2 -> Mixer 1/2 |
| 1 | Software return 3/4 -> Mixer 3/4 |
| 0 | Software return 3/4 -> Mixer 1/2 |

The proven startup value is `0x00000006`, which establishes the straight pair-to-pair routing. Runtime commands rewrite the complete cached quadlet rather than issuing an isolated unknown-state update.

`SRC_ANA_OUT` remains visible in `routing get` because the engine already owns its known startup value, but output-source changes are not enabled in this increment. `MIX_ANA_DIG_IN`, `SRC_HP_OUT`, gain, pan and AUX registers also remain untouched.

## Command surface

```bash
fw1814ctl mixer get
fw1814ctl mixer-route get sw1/2|sw3/4 1/2|3/4
fw1814ctl mixer-route set sw1/2|sw3/4 1/2|3/4 on|off
fw1814ctl routing get
```

Settings are intentionally runtime-only during validation. Restarting the transport restores the hardware-proven `0x00000006` straight-through baseline.

## First hardware validation

With stereo audio playing through Analog Outputs 1/2:

```bash
fw1814ctl mixer get
fw1814ctl mixer-route set sw1/2 3/4 on
fw1814ctl routing get
fw1814ctl mixer-route set sw1/2 3/4 off
fw1814ctl routing get
```

Expected behavior:

- the initial cache is `0x00000006`;
- enabling the additional route changes it to `0x0000000e` and mirrors software return 1/2 to Mixer 3/4 without removing the original Mixer 1/2 route;
- disabling the additional route restores `0x00000006`;
- audio remains clean throughout and the engine continues running.

Do not test output-source, headphone, analog-input or digital-input routing as part of this step.
