# FW410 original control-panel mixer model

Last updated: 2026-08-29

## Why this note exists

Two screenshots of the original M-Audio FireWire 410 control panel clarified the meaning of the 7x5 AV/C processing-mixer matrix. This note records the visible UI, Linux reference behavior, macfw hardware tests and the resulting production rules so future work does not depend on having those screenshots available again.

## Original Mixer page

The original **Mixer** page has seven source strips:

1. `1/2 sw rtn`
2. `3/4 sw rtn`
3. `5/6 sw rtn`
4. `7/8 sw rtn`
5. `spdif sw rtn`
6. `analog in`
7. `spdif in`

These correspond to the seven sources decoded from `snd-firewire-ctl-services` and represented by `Fw410MainMixerModel`:

| Model index | Original UI source | AV/C source function block | AV/C source channel |
|---:|---|---:|---:|
| 0 | analog in | 0x02 | 0x01 |
| 1 | spdif in | 0x03 | 0x01 |
| 2 | 1/2 sw rtn | 0x01 | 0x01 |
| 3 | 3/4 sw rtn | 0x00 | 0x01 |
| 4 | 5/6 sw rtn | 0x00 | 0x03 |
| 5 | 7/8 sw rtn | 0x00 | 0x05 |
| 6 | spdif sw rtn / software return 9/10 | 0x00 | 0x07 |

At the bottom of each source strip the original UI has five routing buttons:

- `1/2`
- `3/4`
- `5/6`
- `7/8`
- `spd`

These correspond to the five AV/C processing-mixer destinations:

| Model destination | Original UI button | Destination FB | AV/C output channel |
|---:|---|---:|---:|
| 0 | 1/2 | 0x01 | 0x01 |
| 1 | 3/4 | 0x01 | 0x03 |
| 2 | 5/6 | 0x01 | 0x05 |
| 3 | 7/8 | 0x01 | 0x07 |
| 4 | spd | 0x01 | 0x09 |

These five destinations are **mixer buses / mixer assignments**, not direct CoreAudio physical-output selectors. A source can be assigned to multiple buses, exactly as the original panel shows.

The 7x5 matrix is therefore:

```text
                    Mixer bus
Source              1/2   3/4   5/6   7/8   SPDIF
----------------------------------------------------
SW return 1/2        ...   ...   ...   ...    ...
SW return 3/4        ...   ...   ...   ...    ...
SW return 5/6        ...   ...   ...   ...    ...
SW return 7/8        ...   ...   ...   ...    ...
SW return SPDIF      ...   ...   ...   ...    ...
Analog input         ...   ...   ...   ...    ...
SPDIF input          ...   ...   ...   ...    ...
```

Cell encoding confirmed from Linux and hardware testing:

- enabled = `0x0000`
- disabled = `0x8000`

## Original Output page

The original **Output** page is a separate layer. It shows strips for:

- `1/2 out`
- `3/4 out`
- `5/6 out`
- `7/8 out`
- `spdif out`
- `aux`
- `phones`

The physical output-pair strips have a `main` control/button. The phones strip exposes source choices `1/2`, `3/4`, `5/6`, `7/8`, `spd`, and `aux`.

This supports the layered signal flow:

```text
Software returns ----\
Analog input ---------+--> 7 x 5 MAIN MIXER --> mixer buses 1/2,3/4,5/6,7/8,SPDIF
SPDIF input ----------/                              |
                                                     v
                                      output / phones / aux layer
                                                     |
                                                     v
                                           physical connectors
```

macfw has independently decoded physical-output source selectors/levels and headphone/AUX controls in this output layer.

## AV/C packet

The validated main-mixer CONTROL request uses the existing `writeProcessingMixer()` format:

```text
00 08 b8
82 <destination-function-block> 10 04
<input-plug> <input-channel> <output-channel>
03 02 <value-hi> <value-lo>
```

For the normal main mixer, destination function block is `0x01` and the destination channels are `01,03,05,07,09`.

## Linux/original identity state

The logical software-return assignment used by Linux `snd-firewire-ctl-services` is:

```text
SW Return 1/2   -> Mixer Bus 1/2
SW Return 3/4   -> Mixer Bus 3/4
SW Return 5/6   -> Mixer Bus 5/6
SW Return 7/8   -> Mixer Bus 7/8
SW Return SPDIF -> Mixer Bus SPDIF
```

Model indices:

```text
src 2 -> dst 0
src 3 -> dst 1
src 4 -> dst 2
src 5 -> dst 3
src 6 -> dst 4
```

This is represented by `Fw410MainMixerModel::loadOriginalIdentityPreset()`.

On macfw, writing the complete 35-cell identity matrix does **not** kill the audio path, but CoreAudio output 1/2 becomes audible on physical output 3/4. That confirms the matrix itself is coherent while also showing that Linux's logical return numbering cannot be copied directly as macfw's hardware baseline because macfw uses a different raw AMDTP slot order.

## macfw-compatible baseline

The hardware-validated macfw baseline is:

```text
raw SW Return 3/4   -> Mixer Bus 1/2
raw SW Return 5/6   -> Mixer Bus 3/4
raw SW Return 7/8   -> Mixer Bus 5/6
raw SW Return 9/10  -> Mixer Bus 7/8
raw SW Return 1/2   -> Mixer Bus SPDIF
```

Model indices:

```text
src 3 -> dst 0
src 4 -> dst 1
src 5 -> dst 2
src 6 -> dst 3
src 2 -> dst 4
```

The other 30 cells are disabled.

Writing all 35 cells with this matrix succeeds and leaves normal macfw playback functioning. This mapping compensates for macfw's current AMDTP slot ordering; it is **not** being claimed as the original M-Audio logical identity state.

## Why STATUS is not authoritative

Earlier tests found mixer STATUS cells reporting OFF while normal playback still worked. The upstream Linux implementation explicitly avoids using AV/C STATUS to discover this mixer state because the ASIC is sensitive to that traffic. Linux instead maintains cached software state and forces CONTROL operations.

Production rules:

- do not brute-force mixer STATUS polling;
- do not initialize the software model from STATUS results;
- do not assume an OFF STATUS result means the corresponding normal playback path is absent;
- maintain a trusted cached matrix after coherent initialization.

## Hardware validation: coherent initialization is required

An isolated mixer CONTROL write was first tested for:

```text
Analog Input 1/2 -> Mixer Bus 1/2
```

with:

```text
functionBlock = 0x01
inputPlug     = 0x02
inputChannel  = 0x01
outputChannel = 0x01
```

When this one write was issued against the FW410's unknown/default state, normal playback stopped on all outputs. Writing the same cell OFF did not restore audio; physical disconnect/reconnect was required.

The Linux implementation supplied the important lifecycle clue: it establishes a complete known matrix with CONTROL writes before later differential changes.

### Full 35-cell tests

Two complete matrices were tested using 35 sequential CONTROL requests, with no mixer STATUS reads:

1. **Linux/original identity matrix:** all writes succeeded and audio remained alive, but CoreAudio output 1/2 moved to physical 3/4.
2. **macfw-compatible matrix:** all writes succeeded and normal macfw playback remained available.

### Cached incremental-write test

After the complete macfw matrix was established, the previously destructive isolated cell was tested again:

```text
Analog Input 1/2 -> Mixer Bus 1/2
src 0 -> dst 0
```

Turning that cell ON and OFF with individual CONTROL requests no longer disturbed normal playback.

With an external signal injected into the analog inputs:

```text
Analog Input 1/2 -> Mixer 1/2
Input 1 -> physical Output 1
Input 2 -> physical Output 2
```

A second destination test confirmed:

```text
Analog Input 1/2 -> Mixer 3/4
Input 1 -> physical Output 3
Input 2 -> physical Output 4
```

Turning each assignment OFF removed the direct-monitor signal. Multiple simultaneous assignments are also supported.

## Production IPC and CLI

The production control server exposes:

```text
MAIN_MIXER INIT
MAIN_MIXER GET
MAIN_MIXER ROUTE GET <src> <dst>
MAIN_MIXER ROUTE SET <src> <dst> <0|1>
```

Initialization is lazy: the first production main-mixer access establishes the complete macfw-compatible baseline, updates the cached model and marks the cache authoritative for the current transport process.

`fw410ctl` exposes the user-facing route controls. Hardware testing confirmed the full production path:

```text
fw410ctl -> Unix socket -> active transport -> cached mixer state -> AV/C -> FW410
```

## GUI software-return mapping

The first GUI implementation exposed the raw AV/C software-return names directly. Hardware testing in Logic made the rotation obvious: the raw row called `SW Return 3/4` actually controlled CoreAudio playback 1/2, and the other rows followed the same shift.

The GUI now presents logical CoreAudio/Logic names and translates them internally:

```text
GUI / CoreAudio SW Return 1/2   -> raw AV/C sw3/4
GUI / CoreAudio SW Return 3/4   -> raw AV/C sw5/6
GUI / CoreAudio SW Return 5/6   -> raw AV/C sw7/8
GUI / CoreAudio SW Return 7/8   -> raw AV/C sw9/10
GUI / CoreAudio SW Return 9/10  -> raw AV/C sw1/2
```

This is the correct user-facing behavior. The hardware/backend matrix remains in raw FW410 identities; only the GUI/backend argument mapping translates to CoreAudio naming.

Hardware validation after this correction confirmed that all software-return rows behave as expected and can be assigned to multiple mixer buses.

## Current implementation rule

The supported main-mixer path must continue to follow this sequence:

1. establish the complete 35-cell macfw-compatible matrix;
2. store that matrix as trusted software state;
3. never use mixer STATUS to reconstruct it;
4. apply later route changes as differential CONTROL writes against the cached state;
5. present software returns to users in CoreAudio/Logic order, not raw AV/C order.

The GUI must continue to use the normal IPC/`fw410ctl` path rather than directly owning FireWire.

## Historical experiment preservation

The failed first mixer attempt was preserved temporarily in branch:

```text
archive/fw410-mixer-experiment-2026-08-28
```

Its tip was:

```text
ba4d7144dec436f8218ae4735777ff766a10849c
```

The important findings from that experiment are now captured in this document and in the current production code: isolated writes against unknown state are unsafe, complete coherent initialization is safe, and macfw requires the AMDTP-order-compatible baseline above. The archive branch is therefore no longer required as the sole record of those discoveries and may be removed after the documentation cleanup is complete.
