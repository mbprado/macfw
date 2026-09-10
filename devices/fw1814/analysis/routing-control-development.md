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

`SRC_HP_OUT` at offset `0x98` contains two independent 16-bit one-hot fields:

| Field | Physical output | `0x01` | `0x02` | `0x04` |
|---|---|---|---|---|
| bits 15:0 | Headphone Output 1 | Mixer 1/2 | Mixer 3/4 | AUX 1/2 |
| bits 31:16 | Headphone Output 2 | Mixer 1/2 | Mixer 3/4 | AUX 1/2 |

The full-register diagnostic was hardware-validated on 2026-09-09. Both
physical headphone outputs followed Mixer 1/2 with `0x00010001`; changing only
the second field to Mixer 3/4 produced `0x00020001`, and the outputs followed
their respective mixer buses exactly. The engine now establishes
`0x00010001` as its complete headphone startup baseline, allowing safe cached
individual changes. AUX remains disabled in the persistent command surface
until its signal path is tested separately.

The analog-input mixer uses `MIX_ANA_DIG_IN` at offset `0x90`. Only its
hardware-validated low-byte analog routes are exposed:

| Analog input pair | -> Mixer 1/2 | -> Mixer 3/4 |
|---|---:|---:|
| Analog 1/2 | `0x01` | `0x10` |
| Analog 3/4 | `0x02` | `0x20` |
| Analog 5/6 | `0x04` | `0x40` |
| Analog 7/8 | `0x08` | `0x80` |

The engine establishes the complete value zero at startup. This disables every
analog and digital input route before the control socket becomes available and
gives the backend an authoritative cache for differential updates. Every later
write is restricted to the low byte, keeping all digital-input bits zero. The
eight analog cells are included in `routing get`, persistent state and Reset
Defaults.

Pan, digital-input and AUX-level controls remain untouched. Analog monitor
gain work is proceeding one stereo input pair at a time.

The first guarded level diagnostic targeted `GAIN_ANA_12_IN` at offset `0x10`,
which controls the Analog Inputs 1/2 contribution to the monitor mixer. The
upper 16 bits are the left channel and the lower 16 bits are the right channel.
FFADO documents `0x8000` as mute/lowest and `0x0000` as unity/highest, giving
the two complete stereo words `0x80008000` and `0x00000000`.

Hardware testing confirmed that `0x80008000` completely mutes the Analog Inputs
1/2 direct-monitor contribution and `0x00000000` restores it at unity. This is
not the physical preamp gain and does not change the CoreAudio capture signal.
The control always writes both channels together and exposes no raw or
intermediate attenuation values. The engine now writes unity at startup, keeps
an authoritative cache and persists successful Analog Inputs 1/2 changes.

The same full-word mute/unity restriction was then validated for
`GAIN_ANA_34_IN` at offset `0x14`: `0x80008000` completely muted the Analog
Inputs 3/4 direct-monitor signal and `0x00000000` restored it normally. The
engine now establishes unity for both validated pairs at startup, and both
controls use the authoritative cache and persistent state path.

The next guarded diagnostic targets `GAIN_ANA_56_IN` at offset `0x18`. It
performs no startup write and is not persisted until its physical signal path
is validated.

## Command surface

```bash
fw1814ctl mixer get
fw1814ctl mixer-route get sw1/2|sw3/4 1/2|3/4
fw1814ctl mixer-route set sw1/2|sw3/4 1/2|3/4 on|off
fw1814ctl output-state get
fw1814ctl output-source get 1/2|3/4
fw1814ctl output-source set 1/2|3/4 mixer|aux
fw1814ctl headphone-state get
fw1814ctl headphone-source get 1|2
fw1814ctl headphone-source set 1|2 mixer1/2|mixer3/4
fw1814ctl input-mixer get
fw1814ctl input-mixer-route get analog1/2|analog3/4|analog5/6|analog7/8 1/2|3/4
fw1814ctl input-mixer-route set analog1/2|analog3/4|analog5/6|analog7/8 1/2|3/4 on|off
fw1814ctl input-monitor-level get analog1/2|analog3/4|analog5/6
fw1814ctl input-monitor-level set-all analog1/2|analog3/4|analog5/6 mute|unity
fw1814ctl routing get
```

Successful writes to this validated subset are recorded by `fw1814state` in `/Library/Application Support/macfw/fw1814/control-state.conf`. After each native engine reports low-level readiness, the supervisor replays the saved typed `fw1814ctl` commands through the normal transport-owned socket. The state helper rejects unknown command shapes and never accepts raw register addresses or values.

With no saved overrides, a new engine keeps the hardware-proven
`MIX_ANA_DIG_IN=0x00000000`, `MIX_STM_IN=0x00000006`,
`SRC_ANA_OUT=0x00000000` and `SRC_HP_OUT=0x00010001` startup baseline.
`fw1814state reset` applies and saves all sixteen default routing
cells/selectors plus the validated Analog Inputs 1/2 and 3/4 unity levels.
`fw1814state clear` empties the saved file without changing current hardware
state.

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

## Validated persistence test

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

Observed behavior on 2026-09-09:

- `fw1814state show` contains the two typed overrides;
- after a launchd restart, `routing get` reported `MIX_STM_IN=0x0000000e`
  and `SRC_ANA_OUT=0x00000002`;
- the same non-default values were restored through 44.1 -> 48 kHz and
  48 -> 44.1 kHz transitions;
- physical disconnect/reconnect restored the same values at both 44.1 and
  48 kHz;
- `fw1814state reset` returned the live and saved state to
  `MIX_STM_IN=0x00000006` and `SRC_ANA_OUT=0x00000000`.

This validates the complete persistence lifecycle for the currently enabled
register subset at that checkpoint. Digital-input, gain, pan and AUX-level
controls remain outside the persistent surface.

## Validated headphone-source diagnostic

With playback sent only to Software Return 1/2 and both headphone volumes at a
comfortable level, initialize both physical headphone outputs from Mixer 1/2:

```bash
fw1814ctl headphone-source set-all mixer1/2 mixer1/2
fw1814ctl headphone-state get
```

The resulting cache was `SRC_HP_OUT=0x00010001`, with clean Software Return 1/2
audio on both headphone outputs. Mixer 3/4 was then selected for only the
second headphone output:

```bash
fw1814ctl headphone-source set-all mixer1/2 mixer3/4
fw1814ctl headphone-state get
```

The resulting cache was `SRC_HP_OUT=0x00020001`. Headphone Output 1 remained on
Mixer 1/2 while Headphone Output 2 followed Mixer 3/4 exactly. Both were then
restored to Mixer 1/2. AUX was not tested because its input levels have not yet
been established by macfw.

## Validated individual headphone controls

Confirm the production individual-control and persistence path:

```bash
fw1814ctl routing get
fw1814ctl headphone-source set 2 mixer3/4
fw1814ctl headphone-state get
sudo launchctl kickstart -k system/com.mbprado.macfw.fw1814.transport
fw1814ctl routing get
fw1814ctl headphone-source set 2 mixer1/2
fw1814state show
```

Observed behavior on 2026-09-09:

- startup reported `SRC_HP_OUT=0x00010001`;
- changing Headphone Output 2 produced `SRC_HP_OUT=0x00020001` without
  changing Headphone Output 1, and the selection survived a launchd restart;
- changing Headphone Output 1 produced the reciprocal
  `SRC_HP_OUT=0x00010002` without changing Headphone Output 2, and the
  selection survived a 48 -> 44.1 kHz engine transition;
- both outputs restored independently to Mixer 1/2 and returned the complete
  cache to `SRC_HP_OUT=0x00010001`;
- the persistent state path accepted both typed headphone controls while still
  rejecting the unvalidated AUX source.

Both headphone fields, their differential cached writes and persistence are
now hardware-validated. The headphone AUX source remains disabled.

## Validated analog-input mixer

The guarded diagnostic first established `MIX_ANA_DIG_IN=0x00000000`, then
tested every analog input pair independently against both mixer buses. Each
route was audibly confirmed and returned cleanly to zero:

| Analog input pair | Mixer 1/2 result | Mixer 3/4 result |
|---|---:|---:|
| Analog 1/2 | `0x00000001` | `0x00000010` |
| Analog 3/4 | `0x00000002` | `0x00000020` |
| Analog 5/6 | `0x00000004` | `0x00000040` |
| Analog 7/8 | `0x00000008` | `0x00000080` |

The signal appeared only on the requested mixer bus, host playback remained
clean and every disable operation restored `MIX_ANA_DIG_IN=0x00000000`.
Digital-input bits, gain, pan and AUX were not exercised.

The same session also compared direct hardware monitoring on Mixer 1/2 with
Logic Pro software monitoring returned through Outputs 3/4. The full capture,
CoreAudio and playback path worked correctly, and the observed latency was
almost unnoticeable.

With all eight analog cells proven, the engine now writes the zero baseline at
startup and the typed differential routes use the normal authoritative cache
and persistent state path.

The promoted route was then left enabled as
`MIX_ANA_DIG_IN=0x00000001`. It survived a launchd transport restart, a
48 -> 44.1 kHz transition, the reverse 44.1 -> 48 kHz transition and physical
disconnect/reconnect at 48 kHz. Each new engine restored Analog Inputs 1/2 to
Mixer 1/2 while retaining the other saved routing controls. This completes the
analog-input routing lifecycle validation.

## Analog Inputs 1/2 monitor-level validation

Enable the already-validated direct-monitor route with a known low-level signal
on Analog Input 1. Keep monitor/headphone volume low, then write the complete
stereo mute value:

```bash
fw1814ctl input-mixer-route set analog1/2 1/2 on
fw1814ctl input-monitor-level get analog1/2
fw1814ctl input-monitor-level set-all analog1/2 mute
fw1814ctl input-monitor-level get analog1/2
```

Before promotion, the first `get` rejected the unknown write-only cache. The
mute command reported `GAIN_ANA_12_IN=0x80008000` and silenced only the direct
hardware-monitor contribution. Logic Pro input metering, recording and
software monitoring remained active, distinguishing this mixer attenuation
from the ADC capture path.

The hardware test produced the expected mute word, silenced the direct monitor
signal completely, then produced the expected unity word and restored the
signal normally. This validates the register encoding and physical path.

After promotion, a fresh engine reported the known unity baseline immediately.
Setting mute added the typed `input-monitor-level:analog1/2` entry to
`fw1814state`; a launchd transport restart replayed that entry and the new
engine reported `GAIN_ANA_12_IN=0x80008000`. Writing unity again restored the
signal and replaced the saved mute state. This validates the control's startup,
cache and transport-restart persistence lifecycle.

## Analog Inputs 3/4 monitor-level validation

Enable the validated Analog Inputs 3/4 route with a known low-level signal,
then confirm the new cache begins unknown:

```bash
fw1814ctl input-mixer-route set analog3/4 1/2 on
fw1814ctl input-monitor-level get analog3/4
fw1814ctl input-monitor-level set-all analog3/4 mute
fw1814ctl input-monitor-level get analog3/4
```

The first `get` reported `input-monitor-level-state-uninitialized`. The mute
command reported `GAIN_ANA_34_IN=0x80008000` and completely silenced the direct
hardware-monitor signal.

Restore the complete unity value before ending the test:

```bash
fw1814ctl input-monitor-level set-all analog3/4 unity
fw1814ctl input-monitor-level get analog3/4
```

The unity command reported `GAIN_ANA_34_IN=0x00000000` and restored the direct
monitor signal normally, validating the register encoding and physical path.

After promotion, a fresh engine reported the known Inputs 3/4 unity baseline.
The typed mute appeared in `fw1814state`, survived a launchd transport restart
and returned from the new engine cache as `GAIN_ANA_34_IN=0x80008000`. Writing
unity again restored the direct signal and saved state. This completes the
startup, cache and transport-restart persistence validation for the second
analog input pair.

## Analog Inputs 5/6 monitor-level diagnostic

Enable the validated Analog Inputs 5/6 route with a known low-level signal,
then confirm the new cache begins unknown:

```bash
fw1814ctl input-mixer-route set analog3/4 1/2 off
fw1814ctl input-mixer-route set analog5/6 1/2 on
fw1814ctl input-monitor-level get analog5/6
fw1814ctl input-monitor-level set-all analog5/6 mute
fw1814ctl input-monitor-level get analog5/6
```

The first `get` must report `input-monitor-level-state-uninitialized`. The mute
command must report `GAIN_ANA_56_IN=0x80008000` and silence the direct hardware
monitor signal.

Restore the complete unity value before ending the test:

```bash
fw1814ctl input-monitor-level set-all analog5/6 unity
fw1814ctl input-monitor-level get analog5/6
```

The command must report `GAIN_ANA_56_IN=0x00000000` and restore the direct
monitor signal. Do not restart the transport while Analog Inputs 5/6 are
muted, and do not test individual channels, intermediate attenuation, other
unexposed input pairs, pan or AUX in this pass.
