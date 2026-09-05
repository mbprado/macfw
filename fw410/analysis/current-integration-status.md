# FW410 current integration status

Last updated: 2026-09-05

This document is the canonical handoff for the current CoreAudio/HAL/runtime/control-panel release-candidate state. Older reverse-engineering notes remain useful historical evidence, but this file defines the present integration baseline and immediate next work.

## Executive status

macfw publishes **M-Audio FireWire 410** as a normal macOS CoreAudio device through a dependency-free AudioServerPlugIn and launchd-managed user-space FireWire transport.

Hardware-confirmed:

- native 44.1 kHz and 48 kHz full-duplex operation;
- ten playback channels: Analog Out 1-8 and S/PDIF L/R;
- four capture channels: Analog In 1/2 and S/PDIF In L/R;
- simultaneous playback, recording and Logic software monitoring;
- runtime 44.1 <-> 48 kHz switching;
- physical disconnect/reconnect and guarded bootloader recovery;
- logical CoreAudio-device continuity while the physical transport is offline;
- launchd service restart/recovery and late interface connection after macOS boot;
- low-latency 256-frame capture prefill;
- dedicated isoch callback thread plus dedicated Mach-paced real-time audio service thread at both supported rates;
- native AppKit control panel operation while full-duplex audio remains active;
- headphone, AUX, physical output and complete 7x5 main-mixer routing controls;
- live four-channel input meters;
- Device-tab 44.1/48 kHz selection through the normal CoreAudio/HAL lifecycle;
- Info/diagnostics surface with exact runtime build metadata, Copy Diagnostics and Open Transport Log.

The current architecture is:

```text
macOS application
    -> CoreAudio
    -> macfw AudioServerPlugIn
       -> playback shared ring
       <- capture shared ring
       <- transport status
    -> haltransport launchd supervisor
       -> OFFLINE / RECOVERING / ONLINE status ABI
       -> native-engine READY handshake
       -> native 44.1 or 48 full-duplex engine
          -> normal callback/FCP/control thread
          -> dedicated isoch callback run-loop thread
          -> dedicated Mach-paced real-time audio service thread
          -> local FW410 control IPC
          -> AV/C control transactions
          -> CMP / ISO / AMDTP transport
    -> M-Audio FireWire 410

macfw FW410 Control.app / fw410ctl
    -> /tmp/macfw-fw410-control.sock
    -> control IPC owned by active native engine
```

The HAL and GUI never open FireWire independently. Device boot, rate control, CMP, ISO, AMDTP scheduling, recovery and live AV/C control remain in the transport/service layer.

## Audio/runtime baseline

### Shared transport geometry

Current full-duplex constants:

- playback TX ring: 640 FireWire cycles;
- playback refill half: 320 cycles;
- playback PCM FIFO: 16,384 frames;
- capture receive ring: 256 FireWire cycles;
- receive publication group: 32 cycles;
- capture shared ring: 32,768 frames;
- **capture prefill: 256 frames**.

The previous 4,096-frame capture prefill was an early stability baseline and is no longer current. Hardware testing established 256 frames as the best validated latency/stability point for the present scheduler.

Approximate capture-prefill time:

```text
44.1 kHz: 256 / 44100 ~= 5.8 ms
48 kHz:   256 / 48000 ~= 5.3 ms
```

This is only one internal buffering component and must not be presented as complete CoreAudio or end-to-end latency.

### Real-time scheduling

Both native engines now use the proven scheduling architecture:

```text
normal/control thread
  - normal FireWire callbacks
  - FCP/control IPC

isoch callback thread
  - dedicated CFRunLoop
  - USER_INTERACTIVE QoS

audio service thread
  - capture publication/decode
  - playback SHM -> PCM pumping
  - TX refill/service
  - input meter accumulation
  - USER_INTERACTIVE QoS
  - 250 us Mach pacing
  - THREAD_TIME_CONSTRAINT_POLICY
      period      2000 us
      computation  500 us
      constraint  2000 us
      preemptible  true
```

This architecture was hardware-validated at both rates. 44.1 kHz showed a major reduction in cutoffs while preserving excellent subjective round-trip latency; the same architecture then produced very good 48 kHz behavior with slightly lower perceived latency.

Treat this scheduling path as frozen for the release candidate unless a new reproducible regression requires a change.

### 44.1 kHz startup specifics

44.1 kHz retains its hardware-established post-start AV/C rate reassert before READY. The startup path services audio during the pre-reassert interval so the TX/RX rings are not left unserviced during startup.

A clean 44.1 stop leaves the FW410 at 44.1 rather than forcing an unnecessary restore to 48 kHz. A tested forced 48 -> 44.1 re-arm workaround did not solve the remaining foreground-only startup symptom and was reverted.

Normal launchd-managed operation is the release reference. Foreground `MACFW_VERBOSE=1` runs are not performance-neutral because verbose diagnostics execute on the audio service path and can perturb real-time behavior.

### Rate-switch asymmetry

44.1 -> 48 kHz switching is fast. 48 -> 44.1 kHz is noticeably slower because the 44.1 path currently performs additional established startup work:

- initial AV/C rate transition with settle/readback;
- a larger rate-specific ISO start lead;
- post-start OUTPUT and INPUT 44.1 reassert transactions before READY.

The switch completes and audio returns correctly, so this is a known release non-blocker. Optimize it later with dedicated timing instrumentation rather than modifying the now-stable 44.1 path before release.

## Capture pipeline

The receive path consumes terminal-slot-confirmed 32-cycle NuDCL groups and validates AMDTP/CIP/DBC continuity before publishing four-channel Float32 capture into the persistent shared ring.

CoreAudio input order:

1. Analog In 1
2. Analog In 2
3. S/PDIF In L
4. S/PDIF In R

The transport performs the required raw FW410 AMDTP -> CoreAudio channel permutation.

Current diagnostics include capture frames, queue extrema, shared-ring drops, malformed/invalid packets, completed chunks, DBC gaps, timestamp-back observations, reorder/stale counts, HAL read calls, underrun events and zero-filled frames.

Small `ts-back` increments have not correlated with audible or frame-count failures in the known-good scheduler and are not currently treated as a transport fault by themselves.

## CoreAudio latency reporting

The HAL currently exposes placeholder zero values for `kAudioDevicePropertyLatency`, `kAudioDevicePropertySafetyOffset` and stream latency. These are **not calibrated latency measurements**.

The Device tab therefore displays **Not reported by HAL** rather than formatting those zeros as real latency.

Do not invent latency values from the capture prefill or FireWire ring geometry. Proper CoreAudio latency/safety-offset reporting is deferred until the complete input/output pipeline is measured and mapped to CoreAudio's property semantics.

## Control subsystem

Production control remains transport-owned:

```text
fw410ctl / native GUI
        |
        v
/tmp/macfw-fw410-control.sock
        |
        v
active native transport engine
        |
        v
existing FireWire handle + FCP response space
        |
        v
FW410 AV/C function blocks
```

Validated controls include:

- headphone source: mixer / AUX;
- headphone independent L/R level;
- headphone mixer sources: 1/2, 3/4, 5/6, 7/8, 9/10;
- AUX software-return 1/2 independent L/R level;
- AUX output independent L/R level;
- physical output source selection for Analog 1/2, 3/4, 5/6, 7/8 and S/PDIF;
- independent physical output L/R levels;
- S/PDIF connector state readback;
- main-mixer route assignment for seven sources into five mixer buses.

The AUX path is independent of the five-source headphone mixer. Implemented control state survives 44.1 <-> 48 kHz transitions and normal service lifecycle events.

## Main mixer state model

The validated main mixer is a 7-source x 5-destination assignment matrix.

Sources:

- Analog In 1/2;
- S/PDIF In L/R;
- five software-return stereo pairs.

Destinations:

- Mixer 1/2;
- Mixer 3/4;
- Mixer 5/6;
- Mixer 7/8;
- Mixer S/PDIF.

A route ON is encoded as `0x0000`; OFF is `0x8000`.

Hardware testing established an important state-management rule: issuing one mixer CONTROL write against an unknown/default matrix can disable the normal audio path, while writing a complete coherent 35-cell matrix is safe. The production backend therefore:

1. lazily establishes the complete macfw-compatible 35-cell baseline on first main-mixer access;
2. caches that matrix in the transport process;
3. avoids mixer STATUS polling;
4. performs later route changes as differential CONTROL writes against the trusted cache.

Do not change this rule casually.

Main strip level/pan/mute/AUX-send semantics remain unresolved and are deliberately parked for post-release research. Existing Feature Volume writes/readback were not sufficient to prove the audible signal path and therefore must not be exposed as production controls.

## Native control panel

`fw410/control-panel` is a native AppKit/Objective-C++ application built directly with the standard macOS Command Line Tools.

Current tabs:

- **Mixer** — 7x5 main-mixer assignment matrix;
- **Outputs** — physical-output Mixer/AUX source, L/R level and stereo link;
- **Headphones** — source, L/R volume, five mixer-output pair switches and stereo link;
- **AUX** — software-return 1/2 -> AUX and AUX output stereo levels;
- **Inputs** — live Analog Input 1/2 and S/PDIF L/R meters;
- **Device** — connection state, active/requested sample rate, engine PID, CoreAudio buffer state, 44.1/48 kHz selector and non-calibrated latency status;
- **Info** — GUI/HAL/runtime build identity, system/device information, runtime diagnostics, Copy Diagnostics and Open Transport Log.

The Device sample-rate selector writes `kAudioDevicePropertyNominalSampleRate`; the HAL requests the normal CoreAudio device configuration change, and the supervisor performs the native engine transition. The GUI does not call `rateprobe` directly.

The GUI still uses the validated `fw410ctl` command for established hardware-control actions. This reuses the proven socket/control path while the UI evolves and introduces no extra FireWire ownership.

The Mixer tab presents software returns in CoreAudio/Logic order while translating to the FW410's raw rotated AV/C return identities internally.

## Build/install status

Aggregate targets:

```text
make             -> HAL + release runtime + GUI
make hal         -> HAL only
make runtime     -> installed runtime/control binaries only
make gui         -> control panel only
make all-tools   -> development/reverse-engineering tools
make package     -> complete package including GUI
```

`sudo make install` expects artifacts to have been built as the normal user and installs the HAL bundle, launchd/runtime tree and `/Applications/macfw FW410 Control.app`.

The GUI Makefile now uses a space-free internal bundle path so GNU make does not split the application name into multiple targets. The current GUI build is clean under `-Wall -Wextra -Wpedantic` for the warnings addressed in the release pass.

The runtime installer persists exact release version and Git SHA metadata for the Info/diagnostics page.

## Current functional matrix

| Capability | 44.1 kHz | 48 kHz |
|---|---|---|
| 10-channel playback | validated | validated |
| 4-channel capture | validated | validated |
| simultaneous full duplex | validated | validated |
| Logic software monitoring / physical loopback | validated | validated |
| low-latency real-time scheduler | validated | validated |
| runtime rate switching | validated | validated |
| physical disconnect/reconnect recovery | validated | validated |
| guarded bootloader recovery | validated | validated |
| transport-status / READY lifecycle | validated | validated |
| offline silence + transparent recovery | validated | validated |
| live headphone/AUX controls | validated | validated |
| physical output controls | validated | validated |
| main-mixer assignment routing | validated | validated |
| live input meters | validated | validated |
| Device-tab rate control | validated | validated |
| native GUI controls | validated | validated |

## Known non-blockers / deferred work

- slower 48 -> 44.1 kHz transition;
- calibrated CoreAudio latency/safety-offset reporting;
- unresolved mixer strip level/pan/mute/AUX-send semantics;
- named user presets;
- optional menu-bar status/quick controls;
- separate SIGPIPE hardening remains desirable if the previously observed disconnected-client `signal 13` engine exit is reproduced; do not mix that work into the stable audio baseline without a focused test.

## Final release-candidate regression

Before packaging, validate the installed launchd-managed build, not a verbose foreground transport:

1. clean build and full install;
2. CoreAudio enumeration and default-device usability;
3. 44.1 playback + capture + Logic monitoring loopback;
4. 48 kHz playback + capture + Logic monitoring loopback;
5. 44.1 -> 48 and 48 -> 44.1 switching from the Device tab;
6. repeat the same rate switch once from Audio MIDI Setup;
7. Mixer routing, Outputs, Headphones and AUX controls while audio is active;
8. Inputs meters at both sample rates;
9. persistent control state across rate switches and transport restart;
10. physical disconnect/reconnect recovery;
11. Info runtime metadata, Copy Diagnostics and Open Transport Log;
12. clean `make gui` with no previously reported Makefile/compiler warnings.

Do not merge/package/release until that regression is complete and explicitly approved.

## Quick handoff

> Native 44.1/48 kHz full duplex is hardware-validated with the same dedicated isoch + Mach-paced time-constraint audio scheduling architecture at both rates. Capture prefill is 256 frames. The launchd service path is the authoritative runtime baseline. The transport owns all FireWire and live control IPC. The native GUI now includes Mixer, Outputs, Headphones, AUX, Inputs, Device and Info/Diagnostics; rate selection uses the normal CoreAudio/HAL lifecycle. Mixer strip controls remain parked, 48 -> 44.1 switching is a known slow-but-working non-blocker, and calibrated HAL latency reporting is deferred. The next action is the final regression checklist, then packaging/release after explicit approval.
