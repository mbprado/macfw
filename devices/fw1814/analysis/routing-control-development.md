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

The third bounded test validated `GAIN_ANA_56_IN` at offset `0x18`.
`0x80008000` completely muted the Analog Inputs 5/6 direct-monitor signal and
`0x00000000` restored it normally. The engine now establishes unity for the
first three analog input pairs, and all three controls use the authoritative
cache and persistent state path.

The final bounded test validated `GAIN_ANA_78_IN` at offset `0x1c`.
`0x80008000` completely muted the Analog Inputs 7/8 direct-monitor signal and
`0x00000000` restored it normally. The engine now establishes unity for all
four analog input pairs, and all four controls use the authoritative cache and
persistent state path.

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
fw1814ctl input-monitor-level get analog1/2|analog3/4|analog5/6|analog7/8
fw1814ctl input-monitor-level set-all analog1/2|analog3/4|analog5/6|analog7/8 mute|unity
fw1814ctl routing get
```

Successful writes to this validated subset are recorded by `fw1814state` in `/Library/Application Support/macfw/fw1814/control-state.conf`. After each native engine reports low-level readiness, the supervisor replays the saved typed `fw1814ctl` commands through the normal transport-owned socket. The state helper rejects unknown command shapes and never accepts raw register addresses or values.

### Restore-readiness gate

The initial persistence tests polled the public control socket while the
supervisor was starting a replacement engine. With the saved Analog Inputs 5/6
level set to mute, the first successful read occasionally returned the engine's
unity startup baseline. Waiting two seconds and reading again returned the
correct restored mute state. This proved persistence was working but exposed a
public-readiness race: socket availability preceded completion of state replay.

Supervised engines now gate ordinary control commands with
`ERR control-state-restoring`. `fw1814state` replay processes identify their
typed requests internally and are permitted through the gate. After the replay
attempt completes, the supervisor sends an internal `CONTROL READY` command;
only then can normal clients read or modify the authoritative cache. Standalone
engines do not enable the gate because no supervisor replay follows their
startup.

Hardware validation confirmed the gate with a saved Inputs 5/6 mute value.
Polling every 100 ms observed the socket as unavailable, then received
`ERR control-state-restoring`, and the first successful read already contained
the restored `GAIN_ANA_56_IN=0x80008000` value without an added delay. Unity was
then restored normally. A successful `fw1814ctl engine get`, `routing get` or
other control read is therefore a reliable readiness condition for scripts and
the future GUI.

With no saved overrides, a new engine keeps the hardware-proven
`MIX_ANA_DIG_IN=0x00000000`, `MIX_STM_IN=0x00000006`,
`SRC_ANA_OUT=0x00000000` and `SRC_HP_OUT=0x00010001` startup baseline.
`fw1814state reset` applies and saves all sixteen default routing
cells/selectors plus the validated unity levels for all four analog input pairs.
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

## Analog Inputs 5/6 monitor-level validation

Enable the validated Analog Inputs 5/6 route with a known low-level signal,
then confirm the new cache begins unknown:

```bash
fw1814ctl input-mixer-route set analog3/4 1/2 off
fw1814ctl input-mixer-route set analog5/6 1/2 on
fw1814ctl input-monitor-level get analog5/6
fw1814ctl input-monitor-level set-all analog5/6 mute
fw1814ctl input-monitor-level get analog5/6
```

The first `get` reported `input-monitor-level-state-uninitialized`. The mute
command reported `GAIN_ANA_56_IN=0x80008000` and completely silenced the direct
hardware-monitor signal.

Restore the complete unity value before ending the test:

```bash
fw1814ctl input-monitor-level set-all analog5/6 unity
fw1814ctl input-monitor-level get analog5/6
```

The unity command reported `GAIN_ANA_56_IN=0x00000000` and restored the direct
monitor signal normally, validating the register encoding and physical path.

After promotion, a fresh engine reported the known Inputs 5/6 unity baseline.
The typed mute appeared in `fw1814state` and survived a launchd transport
restart. During rapid post-restart polling, the public interface reported
`ERR control-state-restoring`; its first successful read then returned the
saved `GAIN_ANA_56_IN=0x80008000` value. Writing unity again restored the direct
signal and saved state. This completes the startup, cache, readiness and
transport-restart persistence validation for the third analog input pair.

## Analog Inputs 7/8 monitor-level validation

The final guarded diagnostic used the validated Analog Inputs 7/8 route with a
known low-level signal:

```bash
fw1814ctl input-mixer-route set analog5/6 1/2 off
fw1814ctl input-mixer-route set analog7/8 1/2 on
fw1814ctl input-monitor-level get analog7/8
fw1814ctl input-monitor-level set-all analog7/8 mute
fw1814ctl input-monitor-level get analog7/8
```

The first `get` reported `input-monitor-level-state-uninitialized`. The mute
command reported `GAIN_ANA_78_IN=0x80008000` and completely silenced the direct
hardware-monitor signal.

Restore the complete unity value before ending the test:

```bash
fw1814ctl input-monitor-level set-all analog7/8 unity
fw1814ctl input-monitor-level get analog7/8
```

The unity command reported `GAIN_ANA_78_IN=0x00000000` and restored the direct
monitor signal normally. Inputs 7/8 are now promoted to the known unity startup
baseline, authoritative cache and typed persistent-state path. After promotion,
the initial read returned unity, a typed mute was saved, and rapid polling over
a launchd restart passed through the readiness gate before its first successful
read returned the restored `GAIN_ANA_78_IN=0x80008000` value. Writing unity
again restored both the direct signal and saved state. This completes the
startup, cache, readiness and restart-persistence validation for all four
analog input pairs. Individual channels, pan and AUX remain outside this
bounded pass.

## Analog Inputs 1/2 intermediate-attenuation diagnostic

FFADO maps these registers to AV/C Audio Subunit volume values and updates each
16-bit channel independently. In that signed 8.8 dB representation, -20 dB is
`-20 * 256`, or `0xec00`; the complete linked stereo word is therefore
`0xec00ec00`. The next guarded diagnostic exposes only this single intermediate
value in addition to the already validated mute and unity endpoints. It is not
accepted by `fw1814state` and is not replayed after restart.

With Analog Inputs 1/2 routed to Mixer 1/2 and a steady signal present:

```bash
fw1814ctl input-monitor-level set-all analog1/2 -20db
fw1814ctl input-monitor-level get analog1/2
fw1814ctl input-monitor-level set-all analog1/2 unity
fw1814ctl input-monitor-level get analog1/2
```

Hardware testing confirmed that the diagnostic write reported
`GAIN_ANA_12_IN=0xec00ec00` and reduced the direct-monitor volume normally. The
cache returned the same value. Unity remains the required restore value.

## Analog Inputs 1/2 independent-channel attenuation diagnostic

FFADO updates the upper 16 bits for the left channel and the lower 16 bits for
the right channel. The next diagnostic applies the validated -20 dB value only
to the left member of Analog Inputs 1/2 while preserving the cached right value
at unity. It is restricted to this pair and remains non-persistent.

```bash
fw1814ctl input-monitor-level set-all analog1/2 unity
fw1814ctl input-monitor-channel-level set analog1/2 left -20db
fw1814ctl input-monitor-channel-level get analog1/2 left
fw1814ctl input-monitor-channel-level get analog1/2 right
fw1814ctl input-monitor-level set-all analog1/2 unity
```

Hardware testing confirmed that the left-only write produced
`GAIN_ANA_12_IN=0xec000000` and attenuated physical Input 1 normally while
preserving the other 16-bit field. Updating the right field then produced
`0xec00ec00`, and both input channels were tested successfully. Linked unity
restored `0x00000000`. Independent cached gain-field updates are therefore
validated; arbitrary attenuation ranges and persistence remain deferred.

## Analog Inputs 1/2 pan validation and promotion

FFADO initializes each analog input-pair LR register to `0x7ffe8000`: the upper
left-channel field is hard left (`0x7ffe`) and the lower right-channel field is
hard right (`0x8000`). Center is `0x0000`. The first guarded diagnostic was
limited to Analog Inputs 1/2 and the exact left, center and right positions. It
performed no startup write or persistence.

Hardware testing confirmed the baseline and independent upper/lower field
updates. In addition to the expected one-channel center values `0x00008000`
and `0x7ffe0000`, the following combinations were exercised:

- both channels hard right: `0x80008000`;
- left hard right, right centered: `0x80000000`;
- both channels centered: `0x00000000`.

The audible position followed the selected channel. Writing hard-left to the
left member and hard-right to the right member restores the complete baseline
word `0x7ffe8000`.

That result promotes the bounded three-position control for Analog Inputs 1/2.
The engine now writes the proven left/right baseline during startup, exposes an
authoritative cache immediately after the readiness gate, and persists the two
channel positions independently. `fw1814state reset` records hard-left for the
left member and hard-right for the right member. The production commands are:

```bash
fw1814ctl input-monitor-pan get analog1/2
fw1814ctl input-monitor-pan set analog1/2 left left|center|right
fw1814ctl input-monitor-pan set analog1/2 right left|center|right
```

Continuous intermediate pan values remained a separate hardware-validation
step at this checkpoint.

The first production persistence test set the Inputs 1/2 left member to center,
restarted the launchd transport and polled through the socket-unavailable and
`control-state-restoring` phases. The first successful read already reported
`left-channel=center right-channel=right` and `LR_ANA_12_IN=0x00008000`.
This confirms that pan participates correctly in the established readiness and
state-replay contract. A hard-left write restores the normal `0x7ffe8000`
stereo baseline and saved state.

## Remaining analog input pan validation and promotion

The next guarded step applies the same documented layout to the remaining
analog input-pair registers in one test build:

- `LR_ANA_34_IN` at `0x00700044`;
- `LR_ANA_56_IN` at `0x00700048`;
- `LR_ANA_78_IN` at `0x0070004c`.

The diagnostic build deliberately added no startup writes or persistence. Each
pair was explicitly initialized to the complete known baseline before any
differential write:

```bash
fw1814ctl input-monitor-pan initialize PAIR
fw1814ctl input-monitor-pan get PAIR
```

Here, `PAIR` was `analog3/4`, `analog5/6` and `analog7/8` in turn. With signals
on both physical inputs in each selected pair, both channels were centered and
restored independently:

```bash
fw1814ctl input-monitor-pan set PAIR left center
fw1814ctl input-monitor-pan set PAIR left left
fw1814ctl input-monitor-pan set PAIR right center
fw1814ctl input-monitor-pan set PAIR right right
fw1814ctl input-monitor-pan get PAIR
```

All three registers returned the expected `0x00008000` left-center and
`0x7ffe0000` right-center words. The audible position followed both physical
channels of every pair, and each final read returned the `0x7ffe8000` baseline.

The complete analog pan register family is therefore promoted. Engine startup
writes `0x7ffe8000` to all four registers, and all eight channel positions have
authoritative caches plus typed persistence. Reset Defaults records left for
the first member and right for the second member of every pair. Explicit
diagnostic initialization is no longer required. The production interface is:

```bash
fw1814ctl input-monitor-pan get PAIR
fw1814ctl input-monitor-pan set PAIR left|right left|center|right
```

`PAIR` accepts all four analog input pairs. Continuous intermediate pan values
remain deferred.

The production persistence batch then saved three distinct states:

- Analog Inputs 3/4: left centered, `0x00008000`;
- Analog Inputs 5/6: right centered, `0x7ffe0000`;
- Analog Inputs 7/8: both centered, `0x00000000`.

After a launchd restart, polling passed through socket-unavailable and
`control-state-restoring` responses. The first successful read for every pair
already contained its saved word. A final typed loop restored and saved
`0x7ffe8000` for all four pairs. This completes startup, cache, readiness,
restart-persistence and Reset Defaults coverage for all eight analog pan
channels.

## Intermediate analog pan diagnostic

FFADO exposes LR balance as a signed continuous 16-bit control from `+32766`
(hard left) through zero (center) to `-32768` (hard right). The next bounded
diagnostic tests one midpoint on either side without allowing those unvalidated
values into persistent state:

- half-left: `+16384`, raw `0x4000`;
- half-right: `-16384`, raw `0xc000`.

For each analog input pair, start from the production left/right baseline and
test one member at a time:

```bash
fw1814ctl input-monitor-pan set-test PAIR left half-left
fw1814ctl input-monitor-pan get PAIR
fw1814ctl input-monitor-pan set PAIR left left

fw1814ctl input-monitor-pan set-test PAIR right half-right
fw1814ctl input-monitor-pan get PAIR
fw1814ctl input-monitor-pan set PAIR right right
```

Expected midpoint words are `0x40008000` and `0x7ffec000`; each production
restore must return `0x7ffe8000`. `set-test` changes only the live authoritative
cache and hardware. It does not update `fw1814state`.

Hardware testing confirmed both midpoint words and the corresponding audible
halfway position on every analog input channel. All pairs were returned to the
normal `0x7ffe8000` baseline afterward.

With both endpoints, center and signed midpoints proven, the production API now
exposes normalized continuous pan. User-facing values run from `-100` (hard
left), through `0` (center), to `+100` (hard right); the model converts them to
the documented signed hardware range. Integer percentages are persistent and
replace the saved value for the same pair/channel key:

```bash
fw1814ctl input-monitor-pan set-percent PAIR left|right -100..100
fw1814ctl input-monitor-pan get PAIR
```

The named `left`, `center` and `right` setter remains as a convenient shortcut.
The temporary `set-test` action is removed from the production surface.

An arbitrary asymmetric production state was then validated on Analog Inputs
3/4: left `-25` and right `+35` converted to `LR_ANA_34_IN=0x2000d333`.
After a launchd restart and the normal readiness-gate responses, the first
successful read returned the same `25% left` / `35% right` state and raw word.
This completes continuous conversion, cache and persistence validation. Named
left/right writes restore the usual `0x7ffe8000` baseline.

## Analog input continuous monitor levels

The `-20 dB` AV/C volume value `0xec00` and independent upper/lower gain fields
were previously validated on Analog Inputs 1/2. The same bounded diagnostic
was then tested in one build on Analog Inputs 3/4, 5/6 and 7/8:

```bash
fw1814ctl input-monitor-level set-all PAIR -20db
fw1814ctl input-monitor-level set-all PAIR unity

fw1814ctl input-monitor-channel-level set PAIR left -20db
fw1814ctl input-monitor-channel-level get PAIR left
fw1814ctl input-monitor-channel-level set PAIR left unity

fw1814ctl input-monitor-channel-level set PAIR right -20db
fw1814ctl input-monitor-channel-level get PAIR right
fw1814ctl input-monitor-channel-level set PAIR right unity
```

For every pair, linked attenuation produced `0xec00ec00`, left-only produced
`0xec000000`, right-only produced `0x0000ec00`, and the final linked word
returned to `0x00000000`. The attenuated signal remained clean and audibly
lower on only the selected channel. This validates both 16-bit fields of all
four `GAIN_ANA_*_IN` registers at a non-endpoint value.

The production control now follows the existing `fw410ctl` volume convention:

```bash
fw1814ctl input-monitor-level get PAIR
fw1814ctl input-monitor-level set PAIR <dB|-inf> [<right-dB|-inf>]
```

The accepted range is -128 through 0 dB in whole-dB steps. One value changes
both fields; the optional second value permits independent left/right faders.
`-inf` and `mute` select the AV/C negative-infinity word `0x8000`. Successful
sets replace the pair's typed saved state atomically and are replayed behind
the established control-readiness gate. The older `set-all` mute, unity and
`-20db` forms remain as compatibility shortcuts and now share persistence.
