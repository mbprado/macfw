# M-Audio FireWire 1814

The FW1814 is the second macfw device target.

See [`HISTORY.md`](HISTORY.md) for the main hardware and integration
milestones.

## Current scope

The experimental FW1814 profile currently provides:

- Intel macOS support through Apple's legacy FireWire stack;
- hardware-validated analog full-duplex CoreAudio at 44.1 and 48 kHz;
- Analog Outputs 1-4 and Analog Inputs 1-8;
- rate switching from Audio MIDI Setup;
- automatic boot, transport restart and disconnect/reconnect recovery;
- restoration of the previously selected rate after reconnect;
- a transport-owned routing-control API and authoritative write-only register
  cache for the future GUI;
- experimental runtime assignment of software returns 1/2 and 3/4 to Mixer
  buses 1/2 and 3/4;
- experimental Mixer/AUX source selection for Analog Outputs 1/2 and 3/4;
- hardware-validated, persistent Mixer 1/2 or Mixer 3/4 source selection for
  both physical headphone outputs;
- hardware-validated, persistent routing of the four analog input pairs to
  Mixer 1/2 or Mixer 3/4;
- a guarded, non-persistent stereo mute/unity diagnostic for the Analog Inputs
  1/2 monitor-mixer level;
- hardware-validated persistent restoration of software-return, analog-input,
  analog-output and headphone selections after transport restart, rate changes
  and reconnect.

S/PDIF, ADAT, 88.2/96/176.4/192 kHz, digital-input routing, production level/pan controls, headphone AUX routing and the native control panel remain under development. MIDI is intentionally deferred until the audio/control surface is complete.

## Architecture

FW1814 development is also the beginning of macfw's explicit multi-device layout:

- reusable transport/CoreAudio primitives migrate to `common/` only as they are required by both devices;
- FW1814-specific stream geometry, clock/digital-mode handling, control protocol and GUI live under `devices/fw1814/`;
- the released FW410 implementation remains the regression reference while this extraction happens.

The experimental FW1814 profile now has hardware-validated analog full-duplex
transport and CoreAudio integration at both 44.1 and 48 kHz. Audio MIDI Setup
can switch the nominal rate in either direction, the supervisor selects the
matching transport engine, and disconnect/reconnect recovery restores the
previously selected rate.

See `analysis/dynamic-rate-switching-success.md` for the current validated
checkpoint and `analysis/bringup-plan.md` for the original bring-up sequence.

## Build and install

From the repository root:

```bash
make fw1814
sudo make fw1814-install
```

Build every FW1814 reverse-engineering/diagnostic tool with `make fw1814-tools`. Full requirements, target descriptions, installation and uninstall instructions are centralized in [`../../INSTALL.md`](../../INSTALL.md).

## Routing control API

The active transport owns `/tmp/macfw-fw1814-control.sock`; clients never open FireWire independently. The special-firmware mixer registers cannot be queried safely, so the engine establishes a known startup baseline and caches every successful differential update:

```bash
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" routing get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" mixer get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" mixer-route get sw1/2 1/2
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" mixer-route set sw1/2 3/4 on
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" mixer-route set sw1/2 3/4 off
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-mixer get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-mixer-route get analog1/2 1/2
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-mixer-route set analog1/2 1/2 on
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-mixer-route set analog1/2 1/2 off
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-monitor-level get analog1/2
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-monitor-level set-all analog1/2 mute
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-monitor-level set-all analog1/2 unity
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" output-state get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" output-source get 3/4
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" output-source set 3/4 aux
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" output-source set 3/4 mixer
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-state get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-source get 2
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-source set 2 mixer3/4
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-source set 2 mixer1/2
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" capabilities get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" engine get
"/Library/Application Support/macfw/fw1814/bin/fw1814state" show
"/Library/Application Support/macfw/fw1814/bin/fw1814state" reset
```

Only the hardware-validated analog fields of `MIX_ANA_DIG_IN`, together with
`MIX_STM_IN`, `SRC_ANA_OUT` and `SRC_HP_OUT`, are writable through the
persistent command set. Successful changes are recorded in
`/Library/Application Support/macfw/fw1814/control-state.conf` and replayed
after a new engine reports ready. `fw1814state reset` applies and saves the
proven straight-through defaults; `clear` removes saved overrides without
changing the current hardware state. Headphone AUX selection remains disabled
pending a separate signal-path test. See
[`analysis/routing-control-development.md`](analysis/routing-control-development.md)
for the enabled subset and validation sequence.

The engine initializes `MIX_ANA_DIG_IN` to the validated zero baseline, with
all analog and digital monitoring routes off. Differential controls expose
only the eight proven analog routes for Inputs 1/2 through 7/8; digital-input
bits remain zero and unavailable.

The Analog Inputs 1/2 monitor-level diagnostic writes the complete documented
stereo word and permits only mute or unity. It controls the input's contribution
to the hardware mixer, not the preamp or CoreAudio capture level. Its cache is
unknown after engine startup, and the diagnostic is not saved by
`fw1814state`.
