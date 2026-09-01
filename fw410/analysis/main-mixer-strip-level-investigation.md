# FW410 main-mixer strip-level investigation

Last updated: 2026-09-01

## Status

**Parked / unresolved.**

The original M-Audio FW410 control panel exposes level faders for the seven main-mixer source strips, but the currently identified AV/C Feature Volume controls do not affect the audible macfw main-mixer signal path despite accepting writes and returning the written values correctly.

Do not expose these controls in the macfw GUI yet. The existing 7 x 5 routing matrix remains the known-good mixer implementation.

## Reference mapping discovered

The old FFADO M-Audio BeBoB mixer implementation associates the FW410 source-strip faders with ordinary `Feature_Volume` controls. The mapping is:

| Source | Feature block | Left channel | Right channel |
| --- | ---: | ---: | ---: |
| Analog In 1/2 | `0x03` | `0x01` | `0x02` |
| S/PDIF In L/R | `0x04` | `0x01` | `0x02` |
| SW Return 1/2 | `0x02` | `0x01` | `0x02` |
| SW Return 3/4 | `0x01` | `0x01` | `0x02` |
| SW Return 5/6 | `0x01` | `0x03` | `0x04` |
| SW Return 7/8 | `0x01` | `0x05` | `0x06` |
| SW Return 9/10 | `0x01` | `0x07` | `0x08` |

This also confirms the channel-number translation used by the Linux implementation: Linux `AudioCh::Each(0)` corresponds to AV/C channel byte `1`, `Each(1)` to channel byte `2`, etc.

FFADO treats these Feature Volume controls separately from the enhanced-mixer routing controls. Its FW410 mixer configuration therefore provides useful evidence that the blocks exist, but it does not by itself prove that these controls affect the same signal path currently used by macfw.

## Read-only hardware probe

A diagnostic command was added:

```text
fw410ctl mixer-strip-level get
fw410ctl mixer-strip-level get <source>
```

The transport reads the known Feature Volume addresses using AV/C STATUS only. It does not invoke the 35-cell main-mixer initialization and does not modify routing.

Initial hardware result after starting the macfw transport:

```text
FW410 main mixer strip levels (read-only):
  Analog In 1/2: left 0 dB (raw 0), right 0 dB (raw 0)
  S/PDIF In L/R: left 0 dB (raw 0), right 0 dB (raw 0)
  SW Return 1/2: left 0 dB (raw 0), right 0 dB (raw 0)
  SW Return 3/4: left 0 dB (raw 0), right 0 dB (raw 0)
  SW Return 5/6: left 0 dB (raw 0), right 0 dB (raw 0)
  SW Return 7/8: left 0 dB (raw 0), right 0 dB (raw 0)
  SW Return 9/10: left 0 dB (raw 0), right 0 dB (raw 0)
```

All fourteen known channels therefore respond successfully to AV/C Feature Volume STATUS requests.

## Narrow write experiment

A deliberately restricted experiment enabled `MAIN_STRIP_LEVEL SET` only for raw SW Return 1/2 (`FB 0x02`, channels `1/2`). No GUI or persistence support was added.

The device accepted and verified several values, including:

```text
0 dB       -> raw 0
-6 dB      -> raw -1536
-60 dB     -> raw -15360
-inf       -> raw -32768
```

Example confirmed sequence:

```text
MAIN_STRIP_LEVEL SET 2 -32768 -32768
-> OK 2 -32768 -32768

mixer-strip-level get sw1/2
-> SW Return 1/2: left -inf (raw -32768), right -inf (raw -32768)
```

The written values are therefore accepted by the FW410 and subsequently returned by STATUS exactly as written.

## Audible hardware tests

Despite successful write/readback, no audible level change was observed in the tested paths.

Tests performed included:

1. Logic playback on output 1/2 while changing SW Return 1/2 level.
2. Analog Input 1/2 monitoring to output/mixer 1/2 while testing the corresponding level path.
3. Logic playback on output 1/2 while testing the SW Return 3/4 relationship as a possible macfw/raw-return remapping issue.
4. Logic playback on output 3/4 while testing SW Return 1/2.
5. SW Return 1/2 was driven as low as AV/C negative infinity (`0x8000` / signed `-32768`) with no audible change in the monitored output.

These tests make a simple CoreAudio/AMDTP return permutation an insufficient explanation. In particular, the lack of an audible effect in the analog-input test suggests the issue is broader than macfw's known software-return rotation.

## Current conclusion

The following points are established:

- the FFADO Feature Volume mapping identifies real FW410 AV/C controls;
- all fourteen mapped channels respond to STATUS;
- at least the SW Return 1/2 feature accepts CONTROL writes;
- STATUS returns the written value correctly, including `-inf`;
- those writes did **not** change the audible level in the tested macfw mixer paths;
- therefore a successful Feature Volume write/readback must not be treated as proof that this is the original panel's effective main-mixer fader path under the current configuration.

The original panel definitely presents main-mixer source faders, so their effective hardware semantics remain unresolved.

## Likely next research direction

When this feature is revisited, investigate the enhanced-mixer implementation and the complete signal topology before performing more writes.

Questions to answer:

1. Does the FW410 enhanced-mixer operation contain gain/coefficient semantics beyond the currently validated boolean route values (`0x0000` on, `0x8000` off)?
2. Does the device require another selector/routing state for the Feature Volume blocks to enter the active monitoring path?
3. Does FFADO perform additional setup before its Feature Volume controls become effective?
4. Can behavior of the original M-Audio control panel be captured or inferred to identify the exact FCP command generated by moving one source fader?
5. Are mute/solo implemented as independent AV/C features, enhanced-mixer state, or GUI behavior around another control?

Do not blindly scan or write unknown feature/function blocks. Continue using Linux/FFADO/original-panel evidence first and validate one semantic class at a time on hardware.

## Code checkpoints

Investigation branch: `feature/fw410-mixer-strip-controls`

Relevant commits:

```text
709cf2987d5edeb4b4dece37bb39b00b70f2a7b2  Expose read-only mixer strip level probe
0d2f590ae44caa8adaf2c80da9b2bfbf56878437  Allow isolated SW 1/2 strip level experiment
```

The write experiment should remain considered diagnostic/experimental rather than production mixer functionality until the audible signal-path semantics are understood.
