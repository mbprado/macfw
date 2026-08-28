# FW410 original control-panel mixer model

## Why this note exists

Two screenshots of the original M-Audio FireWire 410 control panel clarified the meaning of the 7x5 AV/C processing-mixer matrix. This note records the visible UI and the resulting interpretation so future work does not depend on having those screenshots available again.

## Original Mixer page

The original **Mixer** page has seven source strips:

1. `1/2 sw rtn`
2. `3/4 sw rtn`
3. `5/6 sw rtn`
4. `7/8 sw rtn`
5. `spdif sw rtn`
6. `analog in`
7. `spdif in`

These correspond directly to the seven sources decoded from `snd-firewire-ctl-services` and represented by `Fw410MainMixerModel`:

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

These correspond directly to the five AV/C processing-mixer destinations:

| Model destination | Original UI button | Destination FB | AV/C output channel |
|---:|---|---:|---:|
| 0 | 1/2 | 0x01 | 0x01 |
| 1 | 3/4 | 0x01 | 0x03 |
| 2 | 5/6 | 0x01 | 0x05 |
| 3 | 7/8 | 0x01 | 0x07 |
| 4 | spd | 0x01 | 0x09 |

The important interpretation is that these five destinations are **mixer buses / mixer assignments**, not CoreAudio physical-output selectors. A source can be assigned to multiple buses, which is consistent with the original UI exposing five independent routing buttons per source.

Therefore the 7x5 matrix is best understood as:

```text
                    Mixer bus
Source              1/2   3/4   5/6   7/8   SPDIF
----------------------------------------------------
SW return 1/2        ...   ...   ...   ...    ...
SW return 3/4        ...   ...   ...   ...    ...
SW return 5/6        ...   ...   ...   ...    ...
SW return 7/8        ...   ...   ...   ...    ...
SW return SPDIF       ...   ...   ...   ...    ...
Analog input          ...   ...   ...   ...    ...
SPDIF input           ...   ...   ...   ...    ...
```

Each cell is a mixer-bus assignment. From the Linux implementation and our AV/C decoding:

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

This supports a layered signal-flow interpretation:

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

macfw has already independently decoded several controls in this output layer, including physical-output source selectors/levels and the headphone source/mixer controls.

## Consequence for the earlier macfw-remapped preset

An earlier experiment treated mixer destinations as if they were physical playback outputs. Because `full_duplex_shared.h` remaps CoreAudio physical channel order into the FW410 AMDTP stream order, that interpretation produced this candidate matrix:

```text
SW Return 3/4  -> destination 1/2
SW Return 5/6  -> destination 3/4
SW Return 7/8  -> destination 5/6
SW Return 9/10 -> destination 7/8
SW Return 1/2  -> destination SPDIF
```

In model indices this is:

```text
src 3 -> dst 0
src 4 -> dst 1
src 5 -> dst 2
src 6 -> dst 3
src 2 -> dst 4
```

The original control-panel UI makes that interpretation unlikely. The destinations are mixer buses, not direct CoreAudio/physical output assignments. This remapped matrix is therefore retained only as a historical software-only experiment and **must not be treated as the intended hardware initialization**.

## Original/Linux identity preset

The natural software-return assignment shown by the original UI semantics is the same identity-style cached state used by Linux `snd-firewire-ctl-services`:

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

This is now represented by `Fw410MainMixerModel::loadOriginalIdentityPreset()`.

Important: matching the original UI and Linux cached state does **not** yet prove that macfw should write all 35 cells during startup. Earlier hardware experiments showed that writing a complete mixer matrix can alter/break otherwise-correct multichannel playback behavior. Hardware initialization remains intentionally disabled while the relationship between the mixer buses and the normal playback/output path is validated.

## Why STATUS cannot be treated as authoritative state

Earlier tests found mixer STATUS cells reporting OFF while normal playback still worked. The upstream Linux implementation also explicitly avoids using AV/C STATUS to discover this mixer state because repeated STATUS requests heavily load the FW410 ASIC and can time out. Linux instead maintains cached software state and uses CONTROL operations.

Therefore:

- do not brute-force STATUS polling;
- do not initialize the software model from STATUS results;
- do not assume an OFF STATUS result means the corresponding normal playback path is absent;
- keep cached mixer state separate from the physical output routing layer.

## Safe next hardware validation

Before any automatic 35-cell initialization, validate a single known original-control-panel routing function against hardware. A useful test is one source-to-bus assignment whose meaning is visually unambiguous in the original UI, such as `Analog Input 1/2 -> Mixer Bus 1/2`, while observing whether normal software playback remains unchanged.

The goal is to prove that our AV/C processing-mixer cell corresponds to the original control-panel routing button, not to infer physical playback routing from AMDTP channel order.
