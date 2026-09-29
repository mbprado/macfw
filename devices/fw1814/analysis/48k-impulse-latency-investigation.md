# 48 kHz intermittent electrical round-trip latency: per-impulse investigation

Branch: `experiment/fw1814-rolling48`. Diagnostic change only; cause is pending a same-engine fast/slow hardware capture. No adjustment to device/stream latency properties, buffers, reserves, rolling lead/guard, service cadence, capture admission/flush rules, startup settling or recovery safeguards. All six engines still build; only the 48 kHz engine connects transport trace endpoints.

The frozen [evidence directory](evidence/48k-latency-20260927/README.md) contains the original 60 clean 48 kHz probes (~10–14 ms), all 12 earlier slow probes (~90 ms, with a 106.437 ms stopping result), logs, metadata and the original 82.125 ms validation output. `manifest.json` verifies byte-for-byte copies and records original paths. Original `samples/benchmarks` files were not changed. The subsequent 82.625 ms repeat following the background-video discussion is preserved as a session observation, not a fabricated raw file.

The old data cannot locate the delay: 48 kHz does not emit playback/TX/capture loud markers, and lifetime first-loud markers would be stale after the first probe anyway. The 640-cycle TX program repeats every 80 ms, the live lead defaults to 96 cycles (12 ms), capture publishes in 32-slot chunks (~4 ms), and the HAL retains/flushes capture queues according to its existing 4096-frame bound. These are candidates to inspect, not causal findings. Current local launchd configuration explicitly enabled rolling TX; no assumption that it was disabled is justified. The old collection did not record the full per-impulse startup/queue timing.

## Build and install on the Mac

Close Logic, browser/video audio and other audio clients. Physically cable analog output 1 to analog input 1, with direct/mix/software monitoring off to prevent feedback. The diagnostic emits a leading 0.8 impulse followed by a short 0.25 bipolar ID tag; keep the same routing/gain for all probes.

From the repository root:

```sh
git switch experiment/fw1814-rolling48
git pull --ff-only
mkdir -p samples/benchmarks/pre-trace-install
cp /Library/Logs/macfw-fw1814-transport.log samples/benchmarks/pre-trace-install/transport.log
make fw1814 -j2
make -B -C devices/fw1814/hal
make -C devices/fw1814/tools audio-loopback-tool impulse-trace-check
sudo make fw1814-install
```

The normal installer reloads CoreAudio and the supervisor, installs all six engines, and preserves existing launchd tuning variables. It truncates the active service log, hence the explicit pre-install copy above. The forced HAL build refreshes its embedded git identity even when a previously built bundle exists.

Select 48 kHz in the GUI and allow it to settle. Alternatively, before beginning the comparison:

```sh
sudo devices/fw1814/tools/fw1814audioloopback --rate 48000
sleep 4
devices/fw1814/tools/control/fw1814ctl/fw1814ctl engine get
devices/fw1814/tools/control/fw1814ctl/fw1814ctl performance-profile get
sudo -v
```

The engine must report 48000 Hz. The ordinary selection/warmup probe is separate from the tagged comparison. Do not switch modes, profiles or restart the transport during collection. Both the new HAL and new 48 engine must be installed: the runner deliberately refuses unavailable tracing rather than using old markers. The engine and HAL use a separate diagnostic sidecar; existing audio-ring layouts/versions are untouched.

## Run the short comparison

Use a fresh output directory each time:

```sh
python3 devices/fw1814/tools/fw1814latencystates.py collect \
  samples/benchmarks/48k-state-test-01 \
  --probes 40 --idle-gap 0.25 \
  --routing 'analog output 1 -> analog input 1; monitoring off; describe gain settings' \
  --host-load 'Logic and video closed; describe other running apps'
```

It starts/stops a standalone AUHAL client for each probe while keeping the same engine running. The default categories are electrical <=20 ms (fast), >=60 ms (slow), and intermediate. Every reading is retained; slow results are the evidence sought, not exclusions. It stops as soon as both valid states are captured. Forty probes normally take about a minute; if only one state appears it prints **INCONCLUSIVE**, saves everything and exits 2. It cannot manufacture the other state or promise that two states coexist on a particular engine instance. A second pass with another fresh directory or a different idle gap is allowed while keeping the same engine; do not reset to force success.

Example output shape (illustrative values, not a new hardware finding):

```text
01: id=... electrical=11.4 ms wall=16.0 ms state=fast errors=[]
...
08: id=... electrical=82.1 ms wall=88.0 ms state=slow errors=[]

| Stage | Fast ms | Slow ms | Slow − fast ms |
| tool_submit -> hal_submit | ... | ... | ... |
| hal_submit -> pcm_read | ... | ... | ... |
| pcm_read -> tx_prepare | ... | ... | ... |
| tx_prepare -> capture_decode | ... | ... | ... |
| capture_decode -> hal_delivery | ... | ... | ... |
| hal_delivery -> tool_delivery | ... | ... | ... |
Largest observed added interval: ...
RETURN_DIRECTORY=/.../samples/benchmarks/48k-state-test-01
```

The raw probe outputs include `trace-session`, seven `trace-stage` JSON records, coherent counter snapshots before/after, Mach timebase, callback sizes, sample offsets and the original electrical/wall outputs. Missing matches retain explicit status/null intervals. A missing/noisy ID tag is not a substitute fast/slow result. Three no-signals, instrumentation failure, wrong/invalid stage IDs, source-frame mismatch, engine cookie/generation/configuration change, log rotation, Logic reopening or transport faults stop collection. No reset, mode change, recovery or tuning request is issued by this script.

Return the **whole output directory**, even for an inconclusive or failed run, especially:

- `comparison.md`, `results.json`, `results.csv`, `completion.json`;
- every `probe-*-id-*.txt` (raw tool output, including failures);
- `transport.log` and `transport-before.log`;
- `metadata.json`, `commands.json`, engine/profile/process/configuration checks.

For example:

```sh
tar -czf samples/benchmarks/48k-state-test-01.tar.gz \
  -C samples/benchmarks 48k-state-test-01
```

Offline reanalysis never runs hardware commands:

```sh
python3 devices/fw1814/tools/fw1814latencystates.py analyze \
  samples/benchmarks/48k-state-test-01
```

## What is measured

Each probe gets a fresh 32-bit ID. Its leading impulse retains the original onset/electrical timestamp convention. After 31 zero frames, an eight-bit preamble and all 32 ID bits occupy twelve frames each, completing a 512-frame tagged signal. Full physical ID validation is required at capture decode and at delivery to the tool. Validation happens later; every stage stores its **leading-edge** timestamp, not the verification-completion time. IDs are not inferred from loudness alone, and the runner never matches nearest or lifetime markers. Use the automatic runner for fresh IDs, not repeated manual reuse of `--trace-id` values.

Seven endpoints distinguish tool callback submission, actual HAL shared-ring submission, shared-to-local PCM read, packet preparation, capture decode, HAL input read and tool callback delivery. Absolute source-frame checks bind HAL→PCM→TX and capture→HAL, in addition to the audio ID. Records include callback/block sizes, sample offsets, queue depths at the leading-edge block, engine PID, unique birth cookie and FireWire generation. The cookie detects an engine restart even if the bus generation stays unchanged. Whole-probe counter deltas include pre-impulse silence and stopped-client intervals; they do not independently prove audible faults.

The trace is a bounded, separately versioned shared-memory sidecar with one expiring request. The audio thread scans only during an armed diagnostic lease. No logging, allocation or lock is added to the sample scan. Coherent counters use a lock-free sequence snapshot. Capture DCL timestamp/CIP metadata and the current FireWire timer/Mach-time read bracket are saved raw. The existing receive DCL update program is unchanged; [Apple's NuDCL API](https://developer.apple.com/documentation/iokit/iofirewirenudclpoolinterface?language=objc_2) requires timestamped DCLs to be updated after execution, which the existing receive chunks already do.

Field meanings in each stage record:

| Field | Meaning |
|---|---|
| `tick`, `verified_tick` | Mach leading-edge processing time; later full tag validation time |
| `sample_host` | AUHAL leading sample host time for tool endpoints; zero elsewhere |
| `frame`, `offset`, `callback_frames` | Absolute stream/ring frame and offset in the observed block |
| `queue`, `pcm_queue` | Shared/local queued frames before the leading block read/write; `UINT64_MAX` means unmeasured |
| tool `a`, `b` | AudioTimeStamp validity flags and unadjusted callback host time |
| PCM `a` | Local PCM produced-frame origin; add `offset` to bind to TX frame |
| TX `a`, `b`, `c`, `d` | Absolute intended packet ordinal, scheduler progress, estimated intended Mach time, DMA slot |
| capture `a`, `b`, `c`, `d` | Raw receive DCL timestamp, CIP SYT, DBC, decoded-packet counter |
| `cycle_timer`, `cycle_host`, `cycle_uncertainty` | Raw GetCycleTime value, Mach bracket midpoint and bracket width in ticks |
| `last_a`, `last_tick` | Last tagged block metadata/time, used to validate the receive timestamp span |

TX preparation is not actual DMA transmission. Intended TX time is a scheduler-derived estimate, bounded by its timer sample and cycle quantization. The RX arrival estimate accepts a full-timer or compact-OHCI interpretation only when exactly one interpretation fits the first/last tagged packet span and the timer bracket is <=1 ms; unsupported/ambiguous formats remain missing. This is a conservative correlation heuristic, not a claim that the undocumented raw representation is universally known. Raw metadata is retained for inspection. No new DCL callbacks/update lists or DMA program changes were added.

## How the comparison guides the next change

A larger tool→HAL interval locates output-side CoreAudio handoff. HAL→PCM or PCM→TX growth locates delay before packet preparation. Decode→HAL growth with larger capture queue depth locates capture backlog/admission. HAL→tool growth locates input delivery. TX preparation→decode growth needs the intended packet timing and validated RX arrival estimate to separate scheduling/device/wire time from receive publication/decode. If the remaining gap is between intended TX and RX arrival, actual TX transmission was not observed: it is not enough to distinguish a missed TX slot from device/wire delay conclusively.

Electrical time is computed from AUHAL sample host timestamps; wall time is the callback processing interval. The report also prints both sample-host adjustments and the electrical/wall intervals reconstructed from stage points. A change confined to sample-host adjustments suggests timestamp accounting rather than a corresponding physical stage delay. The 16/20 ms wall clusters are analyzed separately; with 192-frame callbacks their separation is one 4 ms callback period, not an explanation for ~70 ms added electrical latency.

After the Mac capture, identify the added stage and relevant counter/queue/configuration change. Then propose the smallest fix to that stage, with a fast/slow regression and six-mode safeguards, **before changing behavior**. If timing remains ambiguous, request the specific missing observation rather than changing reserves, lead, cadence or reported latency by intuition.

## Local validation

HAL, loopback tool and all six engines compile with warnings enabled. Hardware-free C++ tests cover exact ID validation, stale ID rejection, split buffers, analog polarity/filter tolerance and cookie/lease invalidation. Eight Python tests cover interval attribution, raw-preserving same-engine collection with no rate/profile/restart commands, missing/stale IDs and partial raw records, same-generation restarts, frame bindings, cross-engine pair rejection, counter resets/coherence and conservative RX timer correlation. The existing benchmark-report tests also remain available locally. No new diagnostic HAL/transport installation or hardware probe was performed in this session; the Mac test is the next evidence step.
