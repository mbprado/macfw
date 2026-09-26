# M-Audio FireWire 1814

The FW1814 is the second macfw device target.

See [`HISTORY.md`](HISTORY.md) for the main hardware and integration
milestones. The architecture, measurements, failed approaches and extension
plan from the completed 44.1/48 kHz low-latency work are consolidated in
[`analysis/single-speed-low-latency-handover.md`](analysis/single-speed-low-latency-handover.md).

## Current scope

The FW1814 analog profile in this branch provides:

- Intel macOS support through Apple's legacy FireWire stack;
- hardware-validated analog full-duplex CoreAudio at 44.1, 48, 88.2 and 96 kHz;
- Analog Outputs 1-4 and Analog Inputs 1-8;
- rate switching from Audio MIDI Setup;
- automatic boot, transport restart and disconnect/reconnect recovery;
- restoration of the previously selected rate after reconnect;
- a transport-owned routing-control API, authoritative write-only register
  cache and native AppKit control panel;
- persistent Aggressive, Balanced and Conservative transport-performance
  profiles for the validated 44.1/48/88.2/96 kHz engines;
- hardware-validated runtime assignment of software returns 1/2 and 3/4 to Mixer
  buses 1/2 and 3/4;
- hardware-validated Mixer/AUX source selection for Analog Outputs 1/2 and 3/4;
- hardware-validated, persistent Mixer 1/2, Mixer 3/4 or AUX source selection
  for both physical headphone outputs;
- hardware-validated, persistent routing of the four analog input pairs to
  Mixer 1/2 or Mixer 3/4;
- hardware-validated continuous stereo level and pan control for all four
  analog input-pair monitor paths;
- hardware-validated continuous software-return, analog-output, headphone,
  AUX-send and AUX-master levels with persistent typed state;
- physical headphone encoders that adjust and save the corresponding output
  gain, with the open control panel following their changes;
- hardware-validated persistent restoration of software-return, analog-input,
  analog-output and headphone selections after transport restart, rate changes
  and reconnect; all four analog input-pair monitor levels also survive a
  transport restart through the same state path.

The 44.1, 48, 88.2 and 96 kHz analog engines now use guarded rolling TX by default.
The dual-speed modes
with 1280- and 640-packet physical allocations respectively, a 96-cycle live
lead and a 48-cycle deadline guard. Fixed half-ring refill remains available
for diagnosis with `MACFW_44_ROLLING_TX=0`, `MACFW_48_ROLLING_TX=0`,
`MACFW_88_ROLLING_TX=0` or `MACFW_96_ROLLING_TX=0`.
Both modes have hardware-tested playback, recording, rate switching, restart
recovery and live performance profiles.
At 88.2 and 96 kHz rolling TX now use a 512-frame READY silence target to
avoid retaining a 4096-frame live PCM queue after a Logic-origin rate change.
Set `MACFW_88_SHORT_READY_RESERVE=0` or
`MACFW_96_SHORT_READY_RESERVE=0` to restore the former target for diagnosis.
44.1 kHz rolling TX already uses a configurable 512-frame live PCM reserve
(`MACFW_44_ROLLING_PCM_RESERVE_FRAMES`, 64..2048); 48 kHz has no READY
silence top-up. Each mode retains its validated startup path.
The normal `make fw1814` and `sudo make fw1814-install` paths include both
modes; older `*-experimental88` and `*-experimental96` targets remain as
compatibility aliases. Minor artifacts have been observed
under heavy host demand; extended stress monitoring remains useful.

The 176.4 and 192 kHz engines remain separate experimental quad-speed work.
The 192 kHz playback and two-channel input monitoring have been hardware-tested.
The validation history, remaining caveats and test commands are in
[`analysis/high-rate-development.md`](analysis/high-rate-development.md).
S/PDIF, ADAT and MIDI are intentionally deferred until macfw can be compared
directly with the original FW410 and FW1814 control panels running
simultaneously.

## Architecture

FW1814 development is also the beginning of macfw's explicit multi-device layout:

- reusable transport/CoreAudio primitives migrate to `common/` only as they are required by both devices;
- FW1814-specific stream geometry, clock/digital-mode handling, control protocol and GUI live under `devices/fw1814/`;
- the released FW410 implementation remains the regression reference while this extraction happens.

The FW1814 analog profile has hardware-tested full-duplex
transport and CoreAudio integration at 44.1, 48, 88.2 and 96 kHz. Audio MIDI Setup
can switch the nominal rate in either direction, the supervisor selects the
matching transport engine, and disconnect/reconnect recovery restores the
previously selected rate.

See `analysis/dynamic-rate-switching-success.md` for the current validated
checkpoint and `analysis/bringup-plan.md` for the original bring-up sequence.

System sleep/wake behavior remains under observation. One non-reproduced
AppleFWOHCI deep-idle kernel panic has been recorded in
[`analysis/power-management-notes.md`](analysis/power-management-notes.md);
no causal link to macfw has been established.

## Build and install

From the repository root:

```bash
make fw1814
sudo make fw1814-install

# Device-specific installer package:
make fw1814-package
```

Build only the control panel with `make fw1814-gui`, or every FW1814 reverse-engineering/diagnostic tool with `make fw1814-tools`. Full requirements, target descriptions, installation and uninstall instructions are centralized in [`../../INSTALL.md`](../../INSTALL.md).

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
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-monitor-level get analog3/4
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-monitor-level set-all analog3/4 mute
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-monitor-level set-all analog3/4 unity
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-monitor-level get analog5/6
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-monitor-level set-all analog5/6 mute
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-monitor-level set-all analog5/6 unity
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-monitor-level get analog7/8
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-monitor-level set-all analog7/8 mute
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" input-monitor-level set-all analog7/8 unity
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" output-state get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" output-source get 3/4
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" output-source set 3/4 aux
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" output-source set 3/4 mixer
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" output-volume get 1/2
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" output-volume set 1/2 -6 -30
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" output-volume set 1/2 0
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-state get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-source get 2
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-source set 2 mixer3/4
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-source set 2 mixer1/2
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-source set 1 aux
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-source set 2 aux
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-volume get 1
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-volume set 1 -6 -30
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" headphone-volume set 1 0
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" aux-send-level get sw1/2
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" aux-send-level set sw1/2 -6 -30
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" aux-send-level set-all sw1/2 mute
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" aux-send-level get analog1/2
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" aux-send-level set analog1/2 -6 -30
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" aux-send-level set-all analog1/2 unity
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" aux-send-level set-all analog1/2 mute
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" aux-output-volume get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" aux-output-volume set -6 -30
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" aux-output-volume set-all mute
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" aux-output-volume set-all unity
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" performance-profile get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" performance-profile set balanced
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" capabilities get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" engine get
"/Library/Application Support/macfw/fw1814/bin/fw1814state" show
"/Library/Application Support/macfw/fw1814/bin/fw1814state" reset
```

During supervised engine startup, ordinary control requests return
`ERR control-state-restoring` until `fw1814state` has finished replaying the
saved controls. The supervisor then publishes control readiness, so the first
successful client read reflects restored authoritative state rather than the
temporary startup baseline. Direct standalone engine runs remain available
immediately because they have no supervisor-managed replay phase.

At 44.1/48/88.2/96 kHz, the performance profiles select the Mach-paced audio service
period: **Aggressive** is 250 µs, **Balanced** is 375 µs, and
**Conservative** is 500 µs. Shorter periods favor latency; longer periods
reduce transport wakeups and CPU use. Profile changes apply live and are saved
through `fw1814state`. Experimental quad-rate engines retain their individually
validated fixed cadence. `MACFW_AUDIO_SERVICE_PERIOD_US` remains an advanced
launchd override (250–2000 µs); while present it is authoritative and the GUI
selector is disabled.

The 44.1/48 kHz engines remain the stable single-speed regression baseline. Their
640-packet allocation is intentionally separate from the 96-cycle live rolling
horizon: shrinking the allocation caused cracked playback, while shortening
only the scheduled horizon retained stability and produced low measured
latency. Dual-speed development should preserve this distinction and retain
its already-validated packet formation. See the low-latency handover linked at
the top of this document before changing high-rate transport geometry.

Only the hardware-validated analog fields of `MIX_ANA_DIG_IN`, together with
`MIX_STM_IN`, `SRC_ANA_OUT` and `SRC_HP_OUT`, are writable through the
persistent command set. Successful changes are recorded in
`/Library/Application Support/macfw/fw1814/control-state.conf` and replayed
after a new engine reports ready. `fw1814state reset` applies and saves the
proven straight-through defaults; `clear` removes saved overrides without
changing the current hardware state. All three documented headphone sources
are hardware-validated and persistent. See
[`analysis/routing-control-development.md`](analysis/routing-control-development.md)
for the enabled subset and validation sequence.

The engine initializes `MIX_ANA_DIG_IN` to the validated zero baseline, with
all analog and digital monitoring routes off. Differential controls expose
only the eight proven analog routes for Inputs 1/2 through 7/8; digital-input
bits remain zero and unavailable.

The four analog input-pair monitor-level controls write their complete
documented stereo words and permit continuous independent L/R attenuation,
including mute and unity. Hardware testing confirmed that they control each
input pair's contribution to the hardware mixer. The engine establishes unity
for all four pairs at startup and successful changes are saved by
`fw1814state`.
