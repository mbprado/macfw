# FW410 persistent control state

Last updated: 2026-08-31

macfw persists user-facing FW410 control changes so mixer/routing/level settings survive native-engine restart, FireWire disconnect/reconnect, and system reboot.

## Architecture

Hardware ownership remains unchanged:

```text
Control.app / fw410ctl
        |
        v
fw410ctl
        |
        +---- successful SET ----> fw410state record
        |                           |
        |                           v
        |        /Library/Application Support/macfw/fw410/control-state.conf
        |
        v
/tmp/macfw-fw410-control.sock
        |
        v
active native FW410 transport engine
        |
        v
AV/C / FireWire
```

The GUI does not write the state file directly. `fw410ctl` records a setting only after the corresponding hardware write has succeeded. This means the same persistence behavior applies whether the change originates in the GUI or from the command line.

During transport recovery:

```text
haltransport starts native engine
        |
        v
native engine acquires FW410 and opens control socket
        |
        v
native engine signals low-level READY
        |
        v
haltransport runs fw410state restore
        |
        v
saved fw410ctl SET operations replay through the normal control socket
        |
        v
transport state published ONLINE
```

Restore failure is non-fatal to audio. A missing, malformed, or temporarily failing saved setting is reported in the transport log, but the audio engine remains available.

## State file

Installed path:

```text
/Library/Application Support/macfw/fw410/control-state.conf
```

Format identifier:

```text
macfw-fw410-control-state-v1
```

Each line stores a stable key plus the corresponding user-facing `fw410ctl` SET arguments. Replaying state therefore goes through the same validation and socket/transport path used interactively.

The file is intentionally writable by normal local users because the control socket itself is a local user-facing control endpoint. The helper does not execute a shell; saved arguments are passed directly to the installed `fw410ctl` binary.

## Persisted controls

All currently supported writable production controls are persisted when changed:

- five physical-output Mixer/AUX source selectors;
- five physical-output stereo levels;
- 7 x 5 main-mixer routing assignments;
- headphone Mixer/AUX source;
- headphone stereo level;
- five headphone mixer-source assignments;
- AUX software-return 1/2 stereo level;
- AUX output stereo level.

Future production writable controls should use the same persistence path when added.

GUI-only interaction state such as a stereo-link button does not represent FW410 hardware state and is not part of this device-state file.

## Main-mixer safety rule during restore

The FW410 main mixer must never be reconstructed from AV/C STATUS polling and isolated mixer writes must not be sent against unknown hardware state.

`fw410state restore` therefore replays saved main-mixer route entries before other saved controls. The first production `MAIN_MIXER` write causes the transport to establish the validated complete 35-cell macfw baseline before applying the saved differential route. Remaining mixer routes then operate against the trusted cached matrix.

This preserves the hardware-validated safety rule documented in `original-control-panel-mixer-model.md`.

## Commands

The installed helper supports:

```bash
fw410state show
fw410state restore
fw410state clear
fw410state reset
```

`show` prints the saved entries without changing hardware.

`restore` replays the saved state through `fw410ctl`.

`clear` removes saved overrides but does not change the current hardware state. After a later reconnect, controls without saved overrides use the normal macfw/device startup state.

`reset` writes and applies the known macfw default control state. The control panel exposes this operation as **Reset Defaults** and asks for confirmation before applying it.

## macfw reset defaults

The reset state is deliberately defined by macfw rather than claiming to reproduce an undocumented factory preset:

- validated macfw 35-cell main-mixer playback baseline;
- analog and S/PDIF input monitor routes off;
- physical output source = Mixer for all five output pairs;
- physical output levels = 0 dB;
- headphone source = Mixer;
- headphone level = 0 dB;
- headphone mixer 1/2 enabled and the other four pairs disabled;
- AUX software-return 1/2 level = 0 dB;
- AUX output level = 0 dB.

The reset dialog is intentionally explicit because resetting levels can cause an audible level change.

## Validation checklist

Before treating persistence as release-ready, hardware-test at least:

1. change one physical output source and level;
2. change headphone source/level and one headphone mixer assignment;
3. change AUX level;
4. enable an analog-input main-mixer route;
5. change one software-return route;
6. confirm `fw410state show` contains those settings;
7. restart the launchd transport and verify restoration;
8. physically disconnect/reconnect the FW410 and verify restoration;
9. reboot macOS and verify restoration;
10. use **Reset Defaults**, confirm the expected baseline, then repeat a transport restart.

Playback/capture must remain stable during all restore tests.