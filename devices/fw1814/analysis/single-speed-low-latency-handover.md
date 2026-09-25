# FW1814 single-speed low-latency handover

## Purpose

This document hands over the hardware-validated low-latency work completed for
the FW1814's **single-speed** modes: 44.1 and 48 kHz. It records not only the
final settings, but the reasoning, failed approaches, diagnostics and safety
rules that should guide the next phase: adapting the same architecture to the
**dual-speed** 88.2 and 96 kHz engines.

In this document, single-speed and dual-speed are AMDTP/BeBoB speed families.
They do not mean simplex/duplex audio: the validated 44.1/48 kHz paths are
full-duplex.

## Validated result

Both released single-speed engines now provide stable CoreAudio playback and
capture, usable software monitoring and live-selectable scheduling profiles.
Representative electrical loopback results on the FW1814 test Mac were:

| Rate | Profile/test state | Timestamp round trip | Callback wall time |
| --- | --- | ---: | ---: |
| 48 kHz | Aggressive, settled engine | about 12-14 ms | about 16-20 ms |
| 44.1 kHz | Aggressive | about 21-23 ms | about 26-30 ms |
| 44.1 kHz | Balanced | about 25.6 ms | about 30.5 ms |

The exact number varies with CoreAudio callback alignment and capture-queue
position. Clean audio, monotonic counters and zero deadline misses matter more
than any single loopback result.

Longer listening tests found clean playback at both rates. Capture can produce
rare small artifacts under deliberate CPU or I/O stress, particularly with
several active channels, but normal use is stable. This remaining sensitivity
is a scheduling/headroom issue rather than sustained CPU exhaustion.

## Final single-speed architecture

### Keep the proven allocation, shorten only the live horizon

The successful design separates the physical NuDCL allocation from the amount
of audio scheduled ahead of the FireWire cursor:

- playback retains the proven 640-packet ring and 320-packet halves;
- rolling TX maintains a 96-cycle live lead, equal to 12 ms at 8000 FireWire
  cycles per second;
- the deadline guard is half the live lead: 48 cycles, or 6 ms;
- missing the safe refill window is fatal to the engine; it stops before
  reusing unsafe slots instead of continuing with potentially corrupt audio;
- capture retains 256 receive slots.

This distinction is fundamental. Reducing the allocated TX ring to 128 packets
looked attractive and measured low latency, but later produced cracked audio.
Restoring the 640-packet allocation recovered stability. Rolling refill then
delivered low latency without sacrificing the proven allocation geometry.

### The 96-cycle boundary is a DMA visibility requirement

A 64-cycle rolling lead stayed operational but still measured roughly 88-90
ms at 48 kHz. The updated packets were apparently written too late for the
active DMA pass and waited for the next 640-packet ring rotation. Moving the
lead to 96 cycles reduced round trip to the expected low-latency range.

Increasing the lead blindly is not a latency fix: the scheduled horizon itself
adds delay, and a large value can merely hide a missed-window problem. Treat
the smallest repeatably visible lead as a hardware/DMA boundary and retain a
separate guard behind it.

### Startup lead is not live latency

The engine may need substantially more headroom while allocating rings,
connecting CMP, starting ISO and performing device control transactions. This
startup-only distance must not be confused with the steady rolling horizon:

- 48 kHz uses a 256-cycle scheduled startup lead;
- 44.1 kHz uses a 1024-cycle rolling startup lead;
- both converge to the independent 96-cycle live rolling horizon.

An early 44.1 kHz attempt used too little startup headroom and repeatedly
exited with `scheduled first cycle is no longer safely ahead`, triggering
supervisor recovery and bus resets. Raising only the startup lead fixed the
reboot loop without increasing steady-state monitoring latency.

### Preserve rate-family packet formation

48 kHz uses its fixed blocking packet formation. Native 44.1 kHz uses the
Linux/ALSA-derived variable blocking cadence and maintains its scheduler state
across rolling refills. The low-latency architecture is shared, but the packet
formation is not.

Do not implement 88.2/96 kHz by copying 48 kHz packet contents. Reuse the
allocation/horizon/guard/service model around the already validated
dual-speed packet generators.

The Linux FireWire/BeBoB implementation was used as a protocol reference, not
as a source of macOS buffering constants. At 44.1/48 kHz the device-to-host
stream is 10 PCM plus one MIDI position and the host-to-device stream is six
PCM plus one MIDI position. Linux's blocking AMDTP scheduler supports the
native 44.1-family cadence and the continuously recycled queue model. macOS
NuDCL visibility, callback timing and safe scheduling distance still had to be
measured independently on the FW1814 hardware.

### Playback admission and capture publication are separate controls

At 44.1 kHz, rolling operation bypasses the legacy two-second live PCM preload.
The final live playback reserve is 512 frames. The large internal PCM capacity
and validated startup silence remain available for safe initialization, but
they are not held in front of live CoreAudio audio during steady operation.

Capture consumer activation is independent:

- 48 kHz retains the established 512-frame capture prefill;
- 44.1 kHz rolling operation uses a 256-frame capture prefill;
- a stopped consumer's backlog is discarded before a fresh prefill, avoiding
  replay of stale capture audio when the client returns.

Queue depth printed by `fw1814audioloopback` is a snapshot, not a direct
latency decomposition. In particular, `transport_capture_frames` can exceed
the measured electrical round trip because producer/consumer positions are
sampled after the impulse test.

## Real-time service model

The isoch callback dispatcher and the PCM/capture/TX service loop have separate
responsibilities:

- a dedicated CFRunLoop thread services FireWire callbacks at
  `QOS_CLASS_USER_INTERACTIVE`;
- a dedicated Mach-paced audio thread services capture decode/publication,
  playback SHM consumption and rolling TX refill;
- the audio thread requests a Mach time-constraint policy and continues with
  QoS if the request is unavailable;
- the hot path avoids allocation and logging;
- `AudioLoopTimingStats` records average/maximum loop, capture, playback and TX
  time plus maximum wake lateness outside per-iteration logging.

Observed service work was normally tens of microseconds per iteration, far
below even the 250 us cadence. The engines used about 13% of one CPU core while
the machine still had substantial total idle time. This showed that the main
risk is wakeup latency and I/O scheduling jitter, not a shortage of aggregate
CPU capacity. Splitting the tightly ordered capture/playback/TX work over more
threads would add synchronization and does not solve the measured bottleneck.

## Persistent performance profiles

The validated engines expose three live profiles:

| Profile | Service period | Intent |
| --- | ---: | --- |
| Aggressive | 250 us | Lowest latency and greatest starvation resistance |
| Balanced | 375 us | Middle compromise for typical systems |
| Conservative | 500 us | Fewer wakeups and lower CPU demand |

The profile changes the service cadence, not packet geometry, TX lead, capture
prefill or CoreAudio buffer size. `MachPacer` accepts a new period live, so the
engine does not restart. The selected profile is persisted by `fw1814state`
and restored through the existing transport-owned control socket.

`MACFW_AUDIO_SERVICE_PERIOD_US=250..2000` remains an expert launchd override.
When present it is authoritative and the GUI selector is disabled. The
installer preserves the recognized tuning variables rather than silently
removing them. Avoid leaving this override set during normal profile tests;
otherwise GUI changes are remembered but cannot affect the active cadence.

Experimental high-rate engines intentionally retain their fixed, individually
validated cadence. Profiles must not be enabled for dual-speed modes until the
same A/B validation is complete there.

## Implementation map and change sequence

| Area | Main files | Responsibility |
| --- | --- | --- |
| 48 kHz engine | `transport/analog48_main.cpp`, `transport/pcm_stream48.h` | Rolling TX, capture/playback service and status counters |
| 44.1 kHz engine | `transport/analog44_main.cpp`, `transport/blocking_pcm_tx44.h` | Native variable cadence, rolling TX, startup/live admission |
| Shared real-time support | `transport/realtime_service.h` | QoS, time constraint, dynamic Mach pacing and timing statistics |
| Capture SHM | `transport/shared_io.h` | Consumer detection, stale-backlog discard and fresh prefill |
| Control socket | `transport/fw1814_control_server.h` | Live profile get/set and override state |
| Persistent state | `tools/control/fw1814state/main.cpp` | Save, restore and reset profile selection |
| CLI | `tools/control/fw1814ctl/main.cpp` | `performance-profile get/set` |
| GUI | `control-panel/Sources/main.mm` | Device-tab profile selector and override-disabled state |
| Measurement | `tools/fw1814audioloopback.cpp` | Rate readiness, impulse timing, markers and queue snapshots |
| Installation | `service/install-service.sh` | Preserve recognized expert tuning across reinstall |

The final profile integration was pushed to the experimental branch as remote
commit `cc168e669ea4fb0a0ab95aa0e48be9c10e08e5d2`. Its immediate development
sequence was:

- `92b5237`: configurable service cadence at 44.1 kHz;
- `2dea950`: the same cadence mechanism at 48 kHz;
- `0efa408`: initial installer preservation attempt;
- `7bcb489`: repair the installer update so it preserves only the intended
  tuning keys correctly;
- `cc168e6`: integrate persistent profiles, GUI/CLI/state control, loopback
  readiness and documentation.

The repaired commit supersedes the initial installer attempt; do not reproduce
the `0efa408` behavior independently.

The final single-speed launchd tuning used during validation was:

```text
MACFW_44_ROLLING_TX=1
MACFW_44_ROLLING_TX_CYCLES=96
MACFW_44_ROLLING_PCM_RESERVE_FRAMES=512
MACFW_48_ROLLING_TX=1
MACFW_48_ROLLING_TX_CYCLES=96
MACFW_VERBOSE=1
```

`MACFW_AUDIO_SERVICE_PERIOD_US` should normally be absent so the persistent
profile is authoritative.

## Measurement and diagnostics

### Loopback tool

`devices/fw1814/tools/fw1814audioloopback` opens the FW1814 through AUHAL,
emits one impulse and detects its electrical return. It reports:

- CoreAudio device and stream latency properties;
- timestamp-derived electrical round trip;
- callback wall-clock round trip;
- output/input marker times;
- playback and capture SHM queue snapshots.

After requesting a new rate, the tool waits for matching playback and capture
SHM rates, active playback and advancing decoded capture frames before opening
the measurement stream. One first 44.1 kHz impulse still failed to return in a
transition stress test even though the following run was clean. Treat one
immediate post-transition failure as a readiness/measurement event and repeat
the probe before diagnosing transport corruption. This reduces the original
readiness race but does not prove that the external electrical loop has already
returned to a measurable state.

### Verbose transport counters

The most useful invariants are:

- `tx-roll-packets` advances continuously;
- `tx-roll-miss=0`;
- `dbc-gap=0`, or no increase during the test;
- `malformed=0`, `invalid=0`, `reorder=0`, `stale=0`;
- `pcm` remains small during live playback instead of accumulating a large
  reserve;
- `queued` remains bounded while a capture consumer is active;
- `rt-loop-us`, `rt-cap-us`, `rt-pb-us`, `rt-tx-us` and `rt-wake-max-us`
  remain comfortably inside the rolling deadline guard.

Counters are cumulative for one engine instance. When reading a log that
contains several starts, identify the latest startup banner or counter reset;
plain `grep ... | tail` can mix the end of an old instance with the beginning
of a new one.

Markers (`marker-pb`, `marker-tx`, `marker-cap` and the tool's output/input
markers) were added to locate delay across HAL playback, TX and capture. They
are diagnostic timestamps, not stable ABI.

### GUI rate-change caveat

The Device tab uses CoreAudio's nominal-rate property. A GUI selection can
return before the supervisor has replaced the engine and the new CoreAudio
stream has completely settled. One test performed immediately after GUI rate
changes measured roughly 90-100 ms at 48 kHz. A clean 44.1 -> 48 kHz cycle and
several subsequent tests returned 12-14 ms with zero rolling misses and zero
DBC gaps. Matching source/installed binary hashes and fresh counter resets
confirmed that no old transport was running.

Do not tune the transport from a single immediate post-GUI measurement. Wait
for the new engine ONLINE state, allow a short settling interval and repeat.
A future GUI refinement may expose a `switching` state until the engine and
CoreAudio path are both ready.

## Applying the philosophy to dual-speed modes

The next phase should port the architecture in controlled layers, not copy all
single-speed constants at once.

1. **Freeze the current dual-speed baseline.** Record hashes, startup banners,
   ring sizes, packet cadence, clean-audio behavior and fixed-cadence loopback
   at both 88.2 and 96 kHz.
2. **Add timing visibility first.** Reuse `AudioLoopTimingStats`, wake-lateness
   reporting and loud-sample markers before changing latency.
3. **Keep validated packet generation.** Preserve each dual-speed engine's
   FDF, DBS, SYT interval, data/NODATA cadence, channel map and startup kick.
4. **Separate allocation from live lead.** Keep the known-clean NuDCL ring and
   add rolling refill around its validated packet builder. Do not shrink the
   allocation as the first experiment.
5. **Use a guarded lead sweep.** Start conservatively, then test candidate
   leads while watching for the same one-ring-lap signature seen with 64 cycles
   at 48 kHz. FireWire cycles remain 125 us, but audio frames per packet differ
   at dual speed; compare both cycles and frames.
6. **Keep startup lead independent.** If construction or the rate-kick control
   path consumes the initial schedule, increase startup headroom only. Do not
   inflate the steady rolling horizon.
7. **Tune playback admission separately.** Reduce live PCM reserve only after
   rolling TX is clean. Test silence-to-audio handoff as well as continuous
   program audio.
8. **Tune capture separately.** Establish a clean dual-speed prefill and verify
   every exposed input. Stress CPU, I/O and window activity before reducing it.
9. **Validate fixed cadences before profiles.** Test 250, 375 and 500 us as
   explicit experiments at both rates. Only then expose profiles, and only if
   their meaning remains consistent with single speed.
10. **Exercise transitions last.** After each rate is stable alone, test
    44.1/48 <-> 88.2/96 in both directions, repeated GUI changes, reconnect,
    service restart and profile persistence.

### Dual-speed acceptance criteria

A dual-speed rolling mode is ready for integration only when:

- repeated cold and warm starts produce clean playback and capture;
- electrical loopback is repeatable and materially below the fixed-ring
  baseline;
- program audio and software monitoring remain clean for an extended run;
- all active capture channels remain correctly ordered;
- rolling deadline misses remain zero;
- DBC gaps, malformed packets, invalid labels, reordering and stale-slot
  counters do not grow during steady operation;
- silence-to-audio and client stop/start do not replay stale capture or retain
  an old playback reserve;
- deliberate CPU/I/O/window stress does not cause persistent defects;
- transitions do not leave a valid-looking but one-ring-late TX state;
- an engine failure stops safely and the supervisor recovers without a reboot
  loop.

## Reproduction commands

Installed profile state:

```bash
ctl="/Library/Application Support/macfw/fw1814/bin/fw1814ctl"
state="/Library/Application Support/macfw/fw1814/bin/fw1814state"

"$ctl" performance-profile get
"$ctl" performance-profile set aggressive
"$state" show | grep performance-profile
```

Loopback at the active released rates:

```bash
devices/fw1814/tools/fw1814audioloopback --rate 48000
devices/fw1814/tools/fw1814audioloopback --rate 44100
```

Latest timing/counter windows:

```bash
log="/Library/Logs/macfw-fw1814-transport.log"
grep "FW1814 out-shared=.*rt-loop-us=" "$log" | tail -5
grep "FW1814-44 .*rt-loop-us=" "$log" | tail -5
```

Installed tuning and binary identity:

```bash
plist="/Library/LaunchDaemons/com.mbprado.macfw.fw1814.transport.plist"
sudo /usr/libexec/PlistBuddy -c "Print :EnvironmentVariables" "$plist"
shasum -a 256 \
  devices/fw1814/transport/fw1814analog48 \
  "/Library/Application Support/macfw/fw1814/bin/fw1814analog48"
```

## Stable baseline and deferred work

The single-speed transport should now be treated as the regression baseline.
Do not change its ring geometry, 96/48 live window, startup leads, PCM reserve,
capture prefill or scheduling policy merely to simplify dual-speed code.
Extract reusable machinery only where behavior remains explicit per rate
family.

Remaining single-speed polish is limited to extended rate-switch stress tests,
possible GUI transition-state feedback and continued observation of rare
capture artifacts under heavy host load. None currently blocks using the same
architecture as the starting point for dual-speed latency work.
