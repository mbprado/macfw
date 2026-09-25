# FW1814 project history

This file records visible project milestones rather than every diagnostic
experiment. Detailed protocol, transport and routing findings remain under
`devices/fw1814/analysis/`.

## 2026-09-25 — Persistent transport performance profiles

The validated 44.1/48 kHz rolling engines now expose three persistent,
live-switchable service cadences: Aggressive at 250 µs, Balanced at 375 µs,
and Conservative at 500 µs. Hardware testing found clean playback/capture at
all three values. The 375 µs profile measured a median 48 kHz electrical
round trip of approximately 14.0 ms (20.0 ms callback wall time); two valid
44.1 kHz runs measured approximately 21.5–23.4 ms.

`fw1814ctl performance-profile get|set` uses the existing transport-owned
socket and `fw1814state` persistence path. The Device tab presents the same
three choices without editing launchd configuration or requiring an
administrator prompt. `MACFW_AUDIO_SERVICE_PERIOD_US` remains an advanced
authoritative override. The loopback diagnostic now waits for matching active
transport shared memory and advancing capture before injecting its impulse
after a rate change, reducing the observed first-run readiness race.

The completed single-speed investigation is now consolidated in
[`analysis/single-speed-low-latency-handover.md`](analysis/single-speed-low-latency-handover.md).
It records the separation between stable NuDCL allocation and the live rolling
horizon, the independent startup leads, capture/playback admission, real-time
service measurements, failed approaches and the staged plan for applying the
same design principles to 88.2/96 kHz dual-speed modes.

Follow-up transition testing also showed that a loopback probe started
immediately after a GUI sample-rate selection can observe a temporary unsettled
state. A subsequent clean 44.1 -> 48 kHz cycle returned repeated 12-14 ms
timestamp round trips with advancing rolling counters, zero rolling misses and
zero DBC gaps. Matching installed/source hashes and fresh counter resets ruled
out an old transport binary. The single-rate engines therefore remain the
stable baseline; GUI transition-state feedback is deferred polish.

## 2026-09-24 — Experimental rolling 48 kHz transmit window

An opt-in rolling transmitter decouples the stable 640-packet NuDCL allocation
from the amount of live audio scheduled ahead of the FireWire cursor. The
validated startup sequence, 640/320 allocation geometry and capture prefill
remain unchanged. `MACFW_48_ROLLING_TX=1` enables the experiment, and
`MACFW_48_ROLLING_TX_CYCLES` can override its 96-cycle (12 ms) default lead.
The engine stops before unsafe slot reuse if it misses the associated
48-cycle deadline guard.

Hardware testing exposed an effective DMA-prefetch boundary between 64 and
96 cycles. A 64-cycle lead remained stable but still measured 88.1–90.3 ms
electrical round trip because updated packets apparently missed the active
DMA pass and waited for the next ring rotation. At 96 cycles, repeated 48 kHz
loopback measurements fell to 14.81–17.98 ms (711–863 frames). Program audio,
capture and software monitoring remained clean and usable in real time, with
no rolling deadline misses, DBC discontinuities, malformed packets, invalid
labels or reordering in the representative run. Rare capture artifacts were
heard only while deliberately stressing host CPU or I/O; the 512-frame
capture prefill is therefore retained for headroom.

## 2026-09-22 — Experimental 192 kHz CoreAudio trial

The hardware-validated 192 kHz blocking transport is now available through a
separate `fw1814-install-experimental192` opt-in. The HAL advertises four analog
outputs and two analog inputs only when its root-owned marker exists. The
supervisor applies the same guarded firmware reboot and validated two-second
post-init settling period used at 176.4 kHz, then starts a dedicated 192 kHz
engine. Unlike the 176.4 kHz path, the engine pre-arms OUTPUT and INPUT at
192 kHz before CMP/ISO setup, waits for authoritative INPUT readback, and still
repeats the M-Audio rate kick after both streams start.

Hardware testing confirmed clean 192 kHz CoreAudio output with acceptable
latency and normal two-channel input monitoring. During active playback the
transport reported nonzero 24-bit PCM with no dangerous TX gaps or FireWire
DBC discontinuities. Capture advanced by exactly 384000 frames per two-second
status interval with no malformed packets, timestamp regressions, reordering
or metadata swaps. Invalid MBLA labels were confined to startup and stopped
increasing before the listener test. A controlled 192 -> 48 -> 192 transition
also returned both engines online; the returning 192 kHz start had no invalid
labels, DBC gaps, malformed packets or metadata swaps. The 192 kHz engine
remains experimental; extended recording, program-audio and broader repeated
cross-rate testing are still needed.

## 2026-09-23 — 48 kHz latency reduction and loopback diagnostic

The released 48 kHz engine's live transmit ring was reduced from 640 packets
(about 80 ms) to 128 packets (about 16 ms), retaining the same four-phase
blocking packet geometry and 64-packet refill halves. Physical CoreAudio
loopback improved from 82.65 ms to approximately 17–22 ms round trip, making
the mode usable for software monitoring and approaching the 9.46 ms result
measured from the connected H5 interface.

The HAL now reports provisional 48 kHz device latency of 400 frames on each
input and output scope; stream latency remains zero. Logic consequently reports
about 22 ms, close to the measured live path. The HAL also flushes any active
capture backlog larger than 4096 frames at every supported rate, preventing a
stopped client from replaying hundreds of milliseconds of stale monitoring
audio. The validated two-second post-init settling period is unchanged.

`fw1814audioloopback` was added as a selectable-device CoreAudio diagnostic.
It reports the actual AUHAL rate, device and stream latency properties,
host-timestamped electrical round-trip latency, and FW1814 shared-ring queue
estimates. The H5 reference measured 9.46 ms at 48 kHz; stale capture queues
are identified explicitly in the FW1814 report.

The same live TX reserve reduction was applied to the experimental 96 kHz
engine. Its 640-packet ring was reduced to 128 packets while retaining the
separate startup reserve and warmup sequence. Loopback improved to 11.8–15.6
ms round trip, and the HAL now reports 750 device frames per direction at
96 kHz. A 640/320 reduction was tested at 176.4 kHz but did not return a clean
physical impulse, so the validated 1280/640 176.4-kHz geometry was restored.

CoreAudio device-latency reporting was extended to 44.1, 88.2, 176.4 and
192 kHz. Provisional values split stabilized electrical loopback measurements
equally between input and output scopes: 1345, 7286, 14920 and 17018 frames
per direction, respectively. The corresponding 44.1/88.2/176.4/192 kHz
round-trip measurements used for reporting were 61.0/165.2/169.2/177.3 ms.
The 44.1-kHz loopback varied from 56.4 to 66.4 ms across recent runs. The
192-kHz value uses an earlier successful loopback; its verification probe did
not return an impulse. The 48 and 96 kHz reports were later updated when their
validated 640-packet TX reserves were restored.

## 2026-09-18 — Experimental 88.2/96 kHz CoreAudio trials

Guarded rate-control and duplex-stream diagnostics established the FW1814's
high-rate packet formats. Separate opt-in CoreAudio engines were developed for
88.2 and 96 kHz; the normal installation still exposes 44.1 and 48 kHz.
At 88.2 kHz, a 1280-packet playback ring made the standalone 440-Hz tone
clear. Receive-slot completion, FireWire cycle rollover handling and guarded
salvage of a group with one stale slot stabilized CoreAudio capture.

The test Mac recorded and played an approximately five-minute song at 88.2
kHz without audible abnormalities. The log showed continuous capture, no
capture drops or overwritten groups, and 37 salvaged groups, two HAL input
underruns and 528 playback silence frames. Rate switching among 44.1, 48,
88.2 and 96 kHz also worked, although a rare first 44.1-kHz start sounded
broken and recovered on a subsequent switch. High-rate engines remain
experimental pending further testing; detailed evidence is in
[`analysis/high-rate-development.md`](analysis/high-rate-development.md).

Guarded CONTROL-only tests subsequently confirmed both 176.4 and 192 kHz:
each accepted OUTPUT then INPUT, returned the requested INPUT STATUS, and
restored the original 48-kHz rate. The offline 32-event packet schedule
passed on the test Mac. The first silent 176.4-kHz duplex test then received
44 matching data packets and 20 NODATA packets in 64 slots, with no TX
underruns, unchanged bus generation and successful PCR/rate restoration.
After the TX lead was anchored following PCM preload, the position-2 440-Hz
tone became clear at 176.4 kHz. All four playback positions kept their
lower-rate analog-output mapping (2->1, 3->2, 0->3, 1->4). A standalone
continuous two-position capture decoder then passed on the test Mac: six
steady half-second windows near 176400 Hz, no DBC gaps, malformed packets
or dropped frames, and successful PCR/rate restoration. The first raw input
position carried the stronger test signal. An initially ambiguous Input 2
result was traced to a generator fault. After correction, Input 2 appeared
only on raw position 1 at -29.66 dBFS while position 0 remained at the noise
floor; Input 1 had already appeared on position 0. The two quad-rate capture
positions are therefore mapped as Input 1 -> 0 and Input 2 -> 1. The guarded
standalone transport provided the basis for opt-in CoreAudio integration.

Hardware routing tests also identified a quad-rate AUX boundary. The AUX bus
remained audible at 88.2/96 kHz but was silent through both headphone and
analog-output AUX selections at 176.4 kHz while mixer routing remained
functional. macfw now reports AUX routing as unavailable at quad rates,
rejects interactive AUX source selection and applies a Mixer 1/2 runtime
fallback without overwriting the saved lower-rate AUX preference. The control
panel disables the unavailable AUX choices and controls at 176.4/192 kHz.

## 2026-09-16 — Physical headphone encoders in the `0.04.003` release

The first unified `0.04.000` release included the FW1814 analog CoreAudio
engine, routing/level controls and its own native panel and installer. Testing
confirmed the combined package selects only connected interface stacks,
including each model's distinct bootloader personality.

The physical headphone knobs initially changed neither the transport's saved
headphone gain nor the open panel's volume sliders. The Linux M-Audio special
control service provided the model: poll the FW1814 meter area for transitions
of the first two encoders, then apply the corresponding gain change to each
headphone output. macfw now polls those encoder bytes through its transport
owner, writes the selected headphone gain, saves the updated stereo level and
refreshes the panel while it is open. Hardware testing confirmed the behavior
and the fix was released as `0.04.003`. The write-only gain registers still
use the transport's authoritative software cache.

## 2026-09-10 — Analog input monitoring matrix and levels validated

The complete analog half of the FW1814 `MIX_ANA_DIG_IN` register was validated
with known signals on all four physical input pairs. Each pair routed
independently to Mixer 1/2 and Mixer 3/4 with the documented low-byte values:

- Analog Inputs 1/2: `0x01` and `0x10`;
- Analog Inputs 3/4: `0x02` and `0x20`;
- Analog Inputs 5/6: `0x04` and `0x40`;
- Analog Inputs 7/8: `0x08` and `0x80`.

Every route was audibly confirmed on the requested mixer bus, unrelated buses
and host playback remained clean, and each route returned to the complete zero
value when disabled. The engine now establishes
`MIX_ANA_DIG_IN=0x00000000` at startup, exposes only the eight proven analog
cells and saves their typed differential controls through `fw1814state`.
Digital-input routing remains disabled.

The promoted Analog Inputs 1/2 -> Mixer 1/2 route was retained across a
launchd transport restart, both 48 -> 44.1 kHz and 44.1 -> 48 kHz transitions,
and physical disconnect/reconnect at 48 kHz. The analog-input matrix therefore
shares the validated routing-state lifecycle used by the earlier output and
headphone controls.

Logic Pro software monitoring was also compared with the direct hardware
monitoring path during this work. Capture through CoreAudio and playback on a
separate output pair operated correctly, with almost unnoticeable observed
latency compared with the direct signal.

Software-return gain was validated across both logical pairs after accounting
for the FW1814 raw stream rotation: public `sw1/2` uses `GAIN_STM_34_IN` and
public `sw3/4` uses `GAIN_STM_12_IN`. Mute and unity affected only the selected
Mac playback return and left physical-input monitoring unchanged. The promoted
continuous control was then tested at left -6 dB and right -30 dB on `sw1/2`,
producing `GAIN_STM_34_IN=0xfa00e200`. The audible result was correct and the
asymmetric setting survived a launchd restart behind the control-readiness
gate before a final unity write restored `0x00000000`.

Work then moved to physical analog-output volume. The documented
`GAIN_ANA_12_OUT` and `GAIN_ANA_34_OUT` registers at offsets `0x08` and `0x0c`
received a generation-checked unity startup baseline. Mute and unity were
validated on both output pairs: each selected physical pair affected both
software playback and direct monitoring, while the other pair remained
unchanged. The output-volume family now exposes continuous independent
left/right attenuation, typed persistence and unity Reset Defaults using the
same proven AV/C gain encoding as the input and software-return faders.
Continuous asymmetric settings on both pairs were audibly correct and
survived a launchd restart before both outputs were returned to saved unity.

The next bounded diagnostic covers the two physical headphone faders. FFADO's
documented HP 1/2 and HP 3/4 volume words at offsets `0x38` and `0x3c` now
receive a generation-checked unity startup baseline and expose nonpersistent
mute/unity controls as Headphone Outputs 1 and 2. Both connectors passed the
endpoint and isolation test. The headphone-volume family now exposes
continuous independent left/right attenuation, typed persistence, readiness-
gated replay and unity Reset Defaults.
Arbitrary independent channel levels were subsequently confirmed on both
connectors and survived a launchd restart before returning to saved unity.

Development then entered the AUX path. The documented AUX master now starts at
unity while the supported software-return and analog-input AUX sends start
muted, producing a deterministic quiet bus. Both logical software-return sends
passed mute/unity routing and isolation tests with the same raw stream rotation
already proven for main software-return gain. They now expose continuous
independent left/right attenuation, typed persistence and readiness-gated
replay. Reset Defaults intentionally records both sends as muted.
Asymmetric continuous values on both returns then survived a readiness-gated
launchd restart with the exact requested raw words, completing that family.

All four analog-input AUX sends were then exposed together as a bounded
mute/unity diagnostic. Isolated hardware validation confirmed that Analog
Inputs 1/2 followed its AUX send while every alternate mixer and software AUX
path was disabled. The complete documented `AUX_ANA_12_IN` through
`AUX_ANA_78_IN` register family now supports continuous independent left/right
attenuation, typed persistence and readiness-gated replay. Reset Defaults
retains the quiet muted baseline for every analog AUX send.
Distinct asymmetric values on all four analog pairs then produced their exact
expected raw words and survived the readiness gate after a launchd restart,
completing the analog-input AUX-send family.

The documented `GAIN_AUX_OUT` AUX master was then exposed as a mute/unity
diagnostic. Hardware testing confirmed that mute silenced the complete isolated
AUX mix and unity restored it. The AUX master now supports continuous
independent left/right attenuation, typed persistence and readiness-gated
replay, with unity retained as its Reset Default.
An asymmetric -6/-30 dB AUX master setting then produced
`GAIN_AUX_OUT=0xfa00e200`, changed both audible channels as requested and
survived a readiness-gated launchd restart. A linked 0 dB write restored the
unity baseline, completing the analog/software AUX path.

With that path established, the documented AUX source in each `SRC_HP_OUT`
field was enabled as a nonpersistent diagnostic. Each physical headphone
connector followed the isolated AUX signal independently, producing
`0x00010004` for Headphone 1 and `0x00040001` for Headphone 2; both together
produced `0x00040004`. AUX now joins Mixer 1/2 and Mixer 3/4 as a persistent
headphone source with readiness-gated replay. A saved AUX selection on both
headphone connectors survived a launchd transport restart and the first
successful post-readiness query returned `0x00040004`. Reset Defaults keeps
Mixer 1/2 selected for both connectors.

The documented `GAIN_ANA_12_IN` stereo word was then tested at its two safest
endpoints. `0x80008000` completely muted the Analog Inputs 1/2 contribution to
the hardware mixer, while `0x00000000` restored the signal at unity. This
confirmed that the register affects direct monitoring rather than the preamp
or CoreAudio capture path. The engine now establishes unity as a known startup
baseline, and the bounded mute/unity control participates in persistent state
and Reset Defaults. A saved mute was replayed successfully after a launchd
transport restart, the authoritative cache returned `0x80008000`, and a final
unity write restored both the signal and saved state.

The same bounded test then validated `GAIN_ANA_34_IN`. Its initial cache was
unknown as intended; `0x80008000` completely muted the Analog Inputs 3/4 direct
monitor signal and `0x00000000` restored it normally. Inputs 3/4 now share the
known unity startup baseline and persistent mute/unity state used by Inputs
1/2. A saved Inputs 3/4 mute was also replayed after a launchd transport
restart, after which unity restored both the signal and saved state.

`GAIN_ANA_56_IN` produced the same hardware result for Analog Inputs 5/6:
the documented mute word silenced the direct-monitor signal completely and
the unity word restored it normally. Inputs 5/6 now join the known unity
startup baseline and persistent state model.

The first immediate post-restart Inputs 5/6 read exposed a readiness race: the
public socket could accept a read at the unity startup baseline before saved
state replay finished. A protocol gate now returns `ERR control-state-restoring`
to normal clients while allowing the supervisor's typed replay commands.
Hardware retesting observed the socket unavailable, then the restoring error,
then a first successful read containing the saved `0x80008000` mute value.
Unity was restored afterward. This gives scripts and the future GUI a reliable
control-readiness contract.

The final bounded test validated `GAIN_ANA_78_IN` for Analog Inputs 7/8.
`0x80008000` completely muted its direct-monitor contribution and
`0x00000000` restored it normally. All four analog input-pair monitor levels
now have a known unity startup baseline, authoritative cache, typed mute/unity
control and persistent state support. A saved Inputs 7/8 mute passed through
the control-readiness gate and was already present in the first successful
post-restart read; writing unity again restored the signal and saved state.
This completes restart-persistence validation for all four pairs.

The first intermediate analog monitor value was then validated on Inputs 1/2.
The linked stereo AV/C -20 dB word `0xec00ec00` reduced the direct-monitor
volume normally and returned identically from the authoritative cache. This
establishes the first non-endpoint fader value. Independent upper- and lower-
16-bit updates were then validated: the left-only value `0xec000000` attenuated
Input 1 normally while preserving Input 2, and updating the right field formed
`0xec00ec00` with both channels attenuated. Linked unity restored the complete
zero word. This provides the backend behavior required for independent GUI
faders and a Link control.

The same intermediate-gain test was then completed in one build for Analog
Inputs 3/4, 5/6 and 7/8. Linked and independent left/right attenuation worked
on every pair: `0xec00ec00`, `0xec000000` and `0x0000ec00` all affected only
the intended direct-monitor channels, remained clean, and returned to
`0x00000000` at unity. With the signed 8.8 dB encoding and every register field
now hardware-validated, `fw1814ctl` exposes the production whole-dB interface
used by `fw410ctl`: linked or independent values from -128 through 0 dB plus
the AV/C `-inf` mute endpoint. These typed settings use the authoritative
cache and persistent state replay.

An asymmetric production value was then validated on Analog Inputs 1/2:
left -6 dB and right -30 dB produced `GAIN_ANA_12_IN=0xfa00e200`, reduced the
two channels by the expected different amounts, and survived a launchd
restart through the control-readiness gate. The first successful read returned
the same raw values, and a linked 0 dB write restored `0x00000000`. This
completes continuous analog monitor-level conversion and persistence.

The next guarded control family covers the two software-return faders shown in
the original control panel. The engine now establishes unity in
`GAIN_STM_12_IN` and `GAIN_STM_34_IN` at startup. A nonpersistent mute/unity
diagnostic is exposed for both pairs before continuous level control is
promoted.

The first hardware write also exposed the FW1814's raw playback-pair rotation.
Muting the FFADO Stream 1/2 gain word did not affect CoreAudio Outputs 1/2;
muting the Stream 3/4 word did. This agrees with the earlier PCM map, where
physical Outputs 1/2 occupy raw positions 2/3. The control translation now
maps logical `sw1/2` to `GAIN_STM_34_IN` and logical `sw3/4` to
`GAIN_STM_12_IN`, keeping the public API in CoreAudio/physical order.

With that translation applied, mute and unity were validated on both logical
software-return pairs. Each control silenced and restored only Mac playback on
the matching physical output pair; analog direct monitoring continued as
expected. The software-return faders now expose the same production whole-dB,
independent-stereo and persistent-state interface as the analog monitor
faders. Reset Defaults establishes both pairs at unity.

The documented `LR_ANA_12_IN` field layout was also validated with both
physical inputs. Hard left, center and hard right use `0x7ffe`, `0x0000` and
`0x8000` respectively, with the left channel in the upper 16 bits and the
right channel in the lower 16 bits. Independent writes produced every expected
test word, including `0x00008000`, `0x7ffe0000`, `0x80008000`, `0x80000000`
and `0x00000000`, and the audible image followed the selected channel. The
engine now establishes the proven `0x7ffe8000` left/right baseline for Analog
Inputs 1/2 at startup and exposes persistent per-channel left/center/right
controls backed by its authoritative cache. A saved left-channel center
position survived a launchd transport restart: polling observed the readiness
gate, and the first successful read returned `0x00008000`.

The matching `LR_ANA_34_IN`, `LR_ANA_56_IN` and `LR_ANA_78_IN` registers were
then validated together. Every pair produced the expected `0x00008000` and
`0x7ffe0000` center transitions, both physical channels followed the requested
audible position, and all final reads returned `0x7ffe8000`. All four analog
input-pair pan registers now receive a known startup baseline and expose
persistent independent left/center/right controls. Continuous pan values
remain bounded follow-up work.

Persistence was validated for the complete pan family in one restart. Saved
states `0x00008000`, `0x7ffe0000` and `0x00000000` were assigned to three
different input pairs. After the readiness gate, the first successful read of
each pair already contained its saved state. A final typed loop returned all
four pairs to the normal `0x7ffe8000` baseline and updated the saved state.

The signed midpoint values were then validated on all eight channels:
`0x4000` positioned a channel halfway left and `0xc000` positioned it halfway
right, with the expected complete words `0x40008000` and `0x7ffec000`. Audible
movement was correct for every pair. The control API now exposes persistent
normalized integer pan from `-100` (left) through zero to `+100` (right),
providing the continuous backend required by the future GUI pan controls.
A saved asymmetric `-25`/`+35` state produced `0x2000d333` and survived a
launchd restart through the control-readiness gate, completing continuous pan
conversion and persistence validation.

## 2026-09-09 — Routing controls, persistence and headphone sources validated

The FW1814 gained its first end-user-style routing control surface. The active
transport remains the sole owner of the FireWire device; `fw1814ctl` sends
typed requests through the transport-owned local socket and never opens the
interface independently.

The initial writable subset covers two documented special-firmware registers:

- `MIX_STM_IN` for assigning Software Returns 1/2 and 3/4 to Mixer buses 1/2
  and 3/4;
- `SRC_ANA_OUT` for selecting the Mixer or AUX source independently for Analog
  Outputs 1/2 and 3/4.

These registers cannot be read back from the FW1814. The engine therefore
establishes a complete known baseline, keeps an authoritative software cache,
checks the FireWire generation around each write and updates the cache only
after a successful transaction.

Hardware testing first enabled Software Return 1/2 on both Mixer 1/2 and Mixer
3/4. The cached `MIX_STM_IN` value changed from `0x00000006` to `0x0000000e`,
audio played cleanly on Analog Outputs 1/2 and 3/4, and disabling the extra
route restored `0x00000006`. Selecting AUX for Analog Outputs 3/4 then changed
`SRC_ANA_OUT` from `0x00000000` to `0x00000002` without affecting Outputs 1/2;
selecting Mixer restored the original path.

Writable routing state is persistent. Successful typed changes are recorded
by `fw1814state` and replayed after the native engine reports ready. A custom
state containing `MIX_STM_IN=0x0000000e` and `SRC_ANA_OUT=0x00000002` survived:

- a launchd transport restart;
- 44.1 -> 48 kHz and 48 -> 44.1 kHz transitions;
- physical disconnect/reconnect at both supported rates.

## 2026-09-23 — Restore validated 48 kHz playback reserve

The 48 kHz transmit ring is restored from 128 packets (16 ms) to the previous
hardware-validated 640-packet geometry (about 80 ms), with 320-packet refill
halves. Playback crackling persisted after reinstall and reboot, while the
transport counters showed no host playback underruns. The earlier physical
loopback measurement for the 640-packet configuration was 82.65 ms round trip,
so the HAL reports 1984 device-latency frames on each 48 kHz input/output
scope. The 48 kHz mode should be retested after installing this rollback.

## 2026-09-23 — Restore validated 96 kHz playback reserve

The experimental 96 kHz transmit ring is restored from 128 packets to its
previous 640-packet geometry, with 320-packet refill halves (80 ms / 40 ms).
The 128-packet configuration repeatedly started with broken playback. The
existing external recording reference measured about 119 ms round trip with
the longer reserve, so the HAL reports 5712 device-latency frames per 96 kHz
input/output scope. The 96 kHz mode needs listener validation after install.

## 2026-09-23 — Capture recovery and latency probe follow-up

After recording was reported to go silent after several seconds at 48 kHz,
the transport was found to enable capture only once. When the HAL discarded a
stale capture backlog and cleared the ring's active flag, the 48 kHz engine did
not re-enable it. The 44.1 kHz path now also tracks that state; 48 kHz now
prefills and reactivates capture after a HAL flush. Logic capture and the
electrical loopback test both worked after the 48 kHz fix. The 48 kHz loopback
measured 87.98 ms; Logic's reported latency was within about 2 ms.

`fw1814audioloopback --rate RATE` now reads the current nominal rate and skips
the rate write and two-second wait when the device is already at `RATE`.
Otherwise it requests the rate and retains the existing settling wait.

At 96 kHz, two consecutive loopback runs measured 80.30 ms round trip while
CoreAudio reported 5712 frames on each device scope (about 119 ms combined),
and Logic displayed about 121 ms. The report remains at 5712 frames per scope
pending better directional latency measurements; the loopback only measures
the combined path. No 96 kHz latency adjustment was made. The two-second
post-init settling period remains unchanged.

`fw1814state reset` applies and records the proven straight-through macfw
baseline without claiming undocumented M-Audio factory-default semantics.

The next documented register, `SRC_HP_OUT`, was then exercised through a
guarded full-register diagnostic. Both physical headphone outputs followed
Mixer 1/2 with `0x00010001`; changing only the second field to Mixer 3/4
produced `0x00020001` and the two outputs followed their assigned mixer buses
exactly. This validated the headphone source bit layout and established
`0x00010001` as the engine startup baseline.

Individual cached controls were then enabled for both headphone outputs.
Headphone Output 2 retained its Mixer 3/4 selection (`0x00020001`) across a
launchd transport restart, while Headphone Output 1 retained its reciprocal
Mixer 3/4 selection (`0x00010002`) across a 48 -> 44.1 kHz engine transition.
Both returned independently to Mixer 1/2 and the complete cache returned to
`0x00010001`. The two validated mixer sources are now part of persistent state
and Reset Defaults. AUX selection remains deferred until its signal path is
established safely.

## 2026-09-08 — Native 44.1 kHz CoreAudio and dynamic rate switching validated

Native 44.1 kHz operation was added without sample-rate conversion. The
blocking AMDTP path uses the FW1814's alternating data/NODATA cadence rather
than assuming the fixed 48 kHz pattern. Hardware capture showed the expected
DBS=11, FDF=0x01 formation, continuous DBC advancement on data packets and
stable NODATA handling.

The decisive playback requirement was an 88,200-frame PCM-backed digital
silence preload before ISO setup. Several superficially equivalent warm-up
arrangements produced distortion; the validated sequence preloads two seconds
of silence and then lets it drain naturally through the ordinary audio service
loop. This ordering is now treated as part of the hardware contract.

All four analog outputs played cleanly with both 440 Hz and boundary-unfriendly
523.25 Hz tones. The latter remains an important regression test because it
exposes packet-ring boundary errors that phase-aligned tones can hide.

The supervisor and AudioServerPlugIn were then made rate-aware. Audio MIDI
Setup can switch the interface between native 44.1 and 48 kHz in either
direction; the supervisor tears down the old engine, performs the validated
recovery lifecycle, initializes the requested rate and starts the matching
native engine. Repeated tone tests and normal YouTube/CoreAudio playback were
clean at both rates. Physical reconnect restored the previously selected rate
and returned clean audio.

## 2026-09-07 — Full-duplex 48 kHz CoreAudio and automatic recovery validated

The production-style `fw1814analog48` engine connected the previously proven
transport to a dedicated FW1814 AudioServerPlugIn. macOS exposed the interface
as a normal CoreAudio device with four analog outputs and eight analog inputs.
Playback, recording and full-duplex operation were validated with normal audio
applications.

Early live playback was distorted even though source PCM and packet cadence
were correct. Expanding the transmit ring to 640 packets with 320-packet
refill halves provided approximately 40 ms of scheduling margin and made
arbitrary-frequency playback clean. Capture uses a 256-slot receive ring with
32-packet publication chunks and the confirmed 10-PCM-plus-MIDI event width.

The service path gained dedicated Mach-paced realtime audio servicing,
generation-safe teardown and a long-running launchd supervisor. A simple
restart was not sufficient after some reconnects because the interface could
return in a persistent broken device-side state. The reliable recovery
sequence became:

1. recover or boot the operational FW1814 personality;
2. perform a guarded product-scoped FireWire bus reset;
3. wait for re-enumeration;
4. initialize the new generation;
5. start a fresh transport engine.

Hardware testing validated clean recovery after FireWire disconnect/reconnect,
macOS reboot and FW1814 power off/on. The additional bus reset is therefore a
required part of the current special-firmware lifecycle, not an optional
startup optimization.

## 2026-09-06 — Analog channel maps and dynamic 48 kHz transport established

Known signals were injected one physical channel at a time to replace inferred
stream names with hardware evidence. The capture path confirmed eight analog
input positions in order, followed by two positions reserved for the digital
pair and the MIDI quadlet. The CoreAudio-facing analog input order is therefore
Analog Inputs 1 through 8.

Playback mapping confirmed all four physical analog outputs. The FW1814's raw
AMDTP PCM order differs from the desired user-facing order, so the transport
explicitly maps:

```text
Analog Output 1 <- raw PCM position 2
Analog Output 2 <- raw PCM position 3
Analog Output 3 <- raw PCM position 0
Analog Output 4 <- raw PCM position 1
```

Documented special-firmware routing connected Software Returns 1/2 and 3/4 to
Mixer buses 1/2 and 3/4, then selected those mixers for Analog Outputs 1/2 and
3/4. Dynamic shared-memory playback and capture endpoints were added around
the generation-safe duplex lifecycle, forming the basis of the later
production engine.

## 2026-09-06 — First successful FW1814 duplex AMDTP capture

The first successful transport required both CMP directions and real timed
host-to-device AMDTP traffic. Connecting only the device-to-host direction did
not make this special firmware transmit, and permanent playback NODATA was not
sufficient. Starting blocking silent playback before capture allowed the
documented OUTPUT control, 100 ms delay and INPUT control sequence to complete.

The device then filled every receive-ring slot with the expected 48 kHz
formation:

- host to device: 6 PCM + 1 MIDI, DBS=7;
- device to host: 10 PCM + 1 MIDI, DBS=11;
- eight events per data-bearing packet;
- NODATA packets held DBC and used their own DBS=2 header form.

This proved that macfw could start the FW1814 special-firmware duplex transport
and receive real device-to-host AMDTP data without issuing unsupported
BridgeCo stream-format commands.

## 2026-09-06 — Hardware identity and guarded boot lifecycle confirmed

Development began with read-only fingerprinting of the local FW1814 in both
bootloader and operational personalities. The observed BridgeCo hardware model
ID was `0x83`; configuration-ROM unit-directory values were `0x10070` for the
bootloader and `0x10071` for the operational device, matching the published
Linux FW1814 reference.

A guarded boot helper was added for the documented 12-byte boot-from-flash cue.
It requires the expected FW1814 product identity and BeBoB information block
before writing to `0xffffc8021000`. The interface re-enumerated successfully as
`FW 1814` after the cue.

The post-boot bus-reset helper initially exposed an important IOFireWireLib
lifecycle rule: `BusReset()` must be called only after `Open()`. With the
correct `Open() -> BusReset() -> Close()` sequence, the reset completed and the
bus generation advanced normally. These conservative identity and generation
rules became the foundation for every later mutating diagnostic and the
automatic recovery supervisor.
