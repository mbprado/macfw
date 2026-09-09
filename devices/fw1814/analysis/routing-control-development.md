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

The first writable macfw subset was `MIX_STM_IN` at offset `0x94`:

| Bit | Route |
|---:|---|
| 3 | Software return 1/2 -> Mixer 3/4 |
| 2 | Software return 1/2 -> Mixer 1/2 |
| 1 | Software return 3/4 -> Mixer 3/4 |
| 0 | Software return 3/4 -> Mixer 1/2 |

The proven startup value is `0x00000006`, which establishes the straight pair-to-pair routing. Runtime commands rewrite the complete cached quadlet rather than issuing an isolated unknown-state update.

The cached route-write path and Software Return 1/2 -> Mixer 3/4 cell were hardware-validated at 48 kHz on 2026-09-09. Enabling that route changed the cached quadlet from `0x00000006` to `0x0000000e`, produced clean simultaneous playback on Analog Outputs 1/2 and 3/4, and restored cleanly to `0x00000006` when disabled. The opposite cross-route remains to be exercised when a convenient Software Return 3/4 source is available.

The next enabled register was `SRC_ANA_OUT` at offset `0x9c`:

| Bit | Output pair | `0` | `1` |
|---:|---|---|---|
| 1 | Analog Outputs 3/4 | Mixer 3/4 | AUX 1/2 |
| 0 | Analog Outputs 1/2 | Mixer 1/2 | AUX 1/2 |

The proven startup value is `0x00000000`, selecting the corresponding mixer bus for both analog output pairs. As with `MIX_STM_IN`, the backend changes the full cached quadlet and restores the exact baseline value when requested.

The Analog Outputs 3/4 selector was hardware-validated at 48 kHz on 2026-09-09. With Software Return 1/2 mirrored to both mixer buses, selecting AUX changed only Outputs 3/4 and produced the expected cached value `0x00000002`. Selecting Mixer restored the signal and returned the register cache to `0x00000000`; Outputs 1/2 remained unaffected throughout.

`MIX_ANA_DIG_IN`, `SRC_HP_OUT`, gain, pan and AUX-level registers remain untouched.

## Command surface

```bash
fw1814ctl mixer get
fw1814ctl mixer-route get sw1/2|sw3/4 1/2|3/4
fw1814ctl mixer-route set sw1/2|sw3/4 1/2|3/4 on|off
fw1814ctl output-state get
fw1814ctl output-source get 1/2|3/4
fw1814ctl output-source set 1/2|3/4 mixer|aux
fw1814ctl routing get
```

Successful writes to this validated subset are recorded by `fw1814state` in `/Library/Application Support/macfw/fw1814/control-state.conf`. After each native engine reports low-level readiness, the supervisor replays the saved typed `fw1814ctl` commands through the normal transport-owned socket. The state helper rejects unknown command shapes and never accepts raw register addresses or values.

With no saved overrides, a new engine keeps the hardware-proven `MIX_STM_IN=0x00000006` and `SRC_ANA_OUT=0x00000000` startup baseline. `fw1814state reset` applies and saves all six default routing cells/selectors. `fw1814state clear` empties the saved file without changing current hardware state.

## Validated mixer test

With stereo audio playing through Analog Outputs 1/2:

```bash
fw1814ctl mixer get
fw1814ctl mixer-route set sw1/2 3/4 on
fw1814ctl routing get
fw1814ctl mixer-route set sw1/2 3/4 off
fw1814ctl routing get
```

Observed behavior at 48 kHz:

- the initial cache was `0x00000006`;
- enabling the additional route changed it to `0x0000000e` and mirrored software return 1/2 to Mixer 3/4 without removing the original Mixer 1/2 route;
- disabling the additional route restored `0x00000006`;
- audio remained clean throughout and the engine continued running.

## Validated output-source test

First mirror Software Return 1/2 to both mixer buses, then switch only Analog Outputs 3/4 between Mixer and AUX:

```bash
fw1814ctl mixer-route set sw1/2 3/4 on
fw1814ctl output-state get
fw1814ctl output-source set 3/4 aux
fw1814ctl routing get
fw1814ctl output-source set 3/4 mixer
fw1814ctl routing get
fw1814ctl mixer-route set sw1/2 3/4 off
```

Observed behavior at 48 kHz:

- Analog Outputs 1/2 remained on their mixer source throughout;
- `SRC_ANA_OUT` changed from `0x00000000` to `0x00000002` when Analog Outputs 3/4 selected AUX;
- Analog Outputs 3/4 changed to the current AUX mix as expected;
- selecting Mixer restored the mirrored signal on Analog Outputs 3/4 and returned `SRC_ANA_OUT` to `0x00000000`;
- the engine remained online and audio on unaffected outputs remained clean.

## Next hardware validation

Set two non-default but already-proven controls, confirm they were recorded, restart the launchd service, and verify that the new engine restores both cached quadlets:

```bash
fw1814ctl mixer-route set sw1/2 3/4 on
fw1814ctl output-source set 3/4 aux
fw1814state show
sudo launchctl kickstart -k system/com.mbprado.macfw.fw1814.transport
fw1814ctl routing get
fw1814state reset
fw1814ctl routing get
```

Expected behavior:

- `fw1814state show` contains the two typed overrides;
- after restart, `routing get` reports `MIX_STM_IN=0x0000000e` and `SRC_ANA_OUT=0x00000002`;
- Analog Outputs 1/2 remain clean while Outputs 3/4 use AUX;
- `fw1814state reset` returns the live and saved state to `MIX_STM_IN=0x00000006` and `SRC_ANA_OUT=0x00000000`;
- the state is restored again after a later rate change or physical reconnect.

Do not test headphone, analog-input, digital-input, gain, pan or AUX-level controls as part of this step.
