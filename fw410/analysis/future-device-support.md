# Future M-Audio FireWire device support

This note records what the FW410 work implies for possible support of other M-Audio FireWire interfaces. It is a planning/reference document, not a compatibility claim: none of the devices below are considered supported until tested on real hardware.

## Current conclusion

The FW410 work has progressed far enough that macfw is no longer starting from zero for another FireWire audio interface. A large reusable macOS stack now exists:

- CoreAudio AudioServerPlugIn/HAL integration;
- shared-memory playback and capture rings;
- native full-duplex AMDTP transport experience;
- FireWire ISO TX/RX and NuDCL scheduling;
- transport supervision and explicit readiness/status ABI;
- rate switching;
- disconnect/reconnect recovery;
- launchd service lifecycle;
- Unix-socket control IPC;
- persistent hardware control state and reset lifecycle;
- native AppKit control-panel architecture;
- installer/package infrastructure and diagnostics.

The amount that can be reused depends strongly on the protocol family of the target interface.

The primary protocol reference should continue to be ALSA's `snd-firewire-ctl-services`, together with the relevant kernel/FFADO implementation where useful. The FW410 development already demonstrated that following the Linux driver's device initialization and control strategy can avoid unnecessary reverse engineering and unsafe assumptions.

## FireWire 1814

**Expected family:** M-Audio/BridgeCo BeBoB.

This is currently the strongest candidate for a second macfw device.

Because it belongs to the same general BeBoB/M-Audio family as the FW410, a substantial part of the existing low-level work may be reusable in addition to the generic macOS infrastructure. The remaining device-specific investigation would include at least:

- Configuration ROM/device identification;
- bootloader/operational personality behavior;
- CMP plug topology;
- supported native sample rates;
- playback and capture stream dimensions;
- AMDTP channel/slot ordering;
- AV/C clock/rate initialization requirements;
- mixer/function-block topology;
- physical output/headphone routing and levels;
- device-specific recovery quirks.

The first hardware experiment should be deliberately small: identify the device, dump its Configuration ROM and plug/stream information, compare it against the FW410 implementation and Linux BeBoB definitions, and determine which existing transport components can run unchanged before introducing device-specific code.

If those results match expectations, FW1814 support should be approached as a new device backend/description rather than a completely independent driver.

## ProjectMix I/O

**Expected family:** M-Audio/BridgeCo BeBoB.

For audio transport, ProjectMix I/O is also a promising candidate for extensive FW410/BeBoB reuse. Its audio bring-up can be treated separately from its control-surface functionality.

A sensible progression would be:

1. identify and initialize the FireWire audio device;
2. establish playback/capture topology and native rate handling;
3. validate CoreAudio full-duplex operation;
4. implement mixer/output controls;
5. investigate the ProjectMix control-surface protocol separately.

The last item is a distinct additional project because ProjectMix includes motorized faders, encoders, buttons, transport controls and displays. Audio support should not be blocked on complete control-surface support.

Conceptually:

```text
ProjectMix FireWire
    |
    +-- CoreAudio audio transport
    +-- M-Audio mixer/device controls
    +-- control-surface / MIDI functionality
```

## ProFire 610 and ProFire 2626

**Expected family:** DICE rather than BeBoB.

These devices remain realistic macfw targets, but they should not be treated as straightforward FW410 ports. The upper macOS architecture is highly reusable, while the device/protocol layer would require a DICE implementation.

Likely reusable components include:

- CoreAudio HAL architecture;
- shared-memory audio transport boundary;
- status ABI;
- supervisor/recovery model;
- launchd integration;
- IPC architecture;
- persistence model;
- control-panel framework;
- packaging/install infrastructure.

Likely new DICE-specific work includes:

- DICE discovery and register-space handling;
- clock/sample-rate control;
- stream configuration;
- transmit/receive channel topology;
- DICE routing/mixer controls;
- device-specific ProFire behavior;
- mapping the resulting streams into macfw's CoreAudio channel model.

This is still substantially better than beginning from an undocumented device: Linux provides mature DICE implementations and device-specific ProFire support that can serve as protocol references.

## Relative development outlook

| Device | Protocol family | Expected macfw reuse | Relative effort |
|---|---|---|---|
| FireWire 1814 | BeBoB / M-Audio | Very high | Low-to-moderate |
| ProjectMix I/O audio | BeBoB / M-Audio | Very high | Low-to-moderate |
| ProjectMix control surface | additional device functionality | Medium | Moderate |
| ProFire 610 | DICE | High upper-stack reuse; new protocol layer | High |
| ProFire 2626 | DICE | High upper-stack reuse; new protocol layer | High |

These are engineering estimates only and must be revised after real-hardware probing.

## Architectural direction

Successful support for a second interface would justify gradually separating macfw's generic infrastructure from FW410-specific behavior. A possible long-term organization is:

```text
macfw/
  core/
    firewire/
    amdtp/
    coreaudio/
    transport/
    status/
    ipc/

  devices/
    bebob/
      fw410/
      fw1814/
      projectmix/

    dice/
      profire610/
      profire2626/

  control-panel/
```

This is a direction, not an immediate refactor plan. The working FW410 code should not be reorganized speculatively. A second physical device should first demonstrate which components are genuinely generic; common code can then be extracted with two validated implementations as evidence.

## Recommended next target

If hardware becomes available, **FireWire 1814** is the preferred next experiment because it offers the best chance to prove that macfw can support multiple devices while reusing both the generic macOS stack and significant BeBoB/M-Audio protocol knowledge.

The initial objective should be discovery and topology comparison, not immediate driver development. That result will establish how close macfw is to a reusable multi-device FireWire audio framework.
