# FW1814 latency versus performance profile — 2026-09-27

Branch: experiment/fw1814-rolling48. HEAD: f7417fe0688ea4bb58e8c1b479d46dd82afffcbf.

## Completed benchmark

**20 successful probes per rate/profile combination; 360 successful probes total.** All three profiles were verified live (250/375/500 microseconds), with the environment override inactive. No no-signal results or transition-high exclusions occurred in these completed groups.

| Rate kHz | Aggressive median ms | Balanced median ms | Conservative median ms |
|---|---:|---:|---:|
| 44.1 | 18.141 | 18.413 | 18.503 |
| 48 | 11.417 | 12.167 | 11.417 |
| 88.2 | 19.047 | 18.685 | 19.229 |
| 96 | 9.312 | 9.229 | 9.396 |
| 176.4 | 22.273 | 22.273 | 22.364 |
| 192 | 7.599 | 7.932 | 7.766 |

## Full statistics

| Rate kHz | Profile | n | Median ms | Mean ms | Min–max ms | SD ms | P95 ms | Callback wall median ms |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 44.1 | aggressive | 20 | 18.141 | 18.467 | 17.959–19.773 | 0.569 | 19.592 | 26.111 |
| 44.1 | balanced | 20 | 18.413 | 18.649 | 17.959–19.592 | 0.508 | 19.410 | 26.125 |
| 44.1 | conservative | 20 | 18.503 | 18.821 | 17.959–20.136 | 0.718 | 20.136 | 26.107 |
| 48 | aggressive | 20 | 11.417 | 11.658 | 9.750–13.917 | 1.223 | 13.583 | 16.040 |
| 48 | balanced | 20 | 12.167 | 12.167 | 10.083–13.583 | 0.967 | 13.583 | 19.942 |
| 48 | conservative | 20 | 11.417 | 11.583 | 9.750–13.083 | 1.220 | 13.083 | 16.027 |
| 88.2 | aggressive | 20 | 19.047 | 18.893 | 17.778–19.773 | 0.574 | 19.773 | 21.794 |
| 88.2 | balanced | 20 | 18.685 | 18.630 | 17.959–19.410 | 0.546 | 19.410 | 21.765 |
| 88.2 | conservative | 20 | 19.229 | 19.211 | 17.959–20.136 | 0.708 | 20.136 | 21.776 |
| 96 | aggressive | 20 | 9.312 | 9.329 | 8.146–10.312 | 0.595 | 10.312 | 12.017 |
| 96 | balanced | 20 | 9.229 | 9.304 | 8.479–10.146 | 0.553 | 10.146 | 12.014 |
| 96 | conservative | 20 | 9.396 | 9.354 | 8.646–10.312 | 0.572 | 10.312 | 12.016 |
| 176.4 | aggressive | 20 | 22.273 | 22.346 | 21.910–24.087 | 0.422 | 22.454 | 23.954 |
| 176.4 | balanced | 20 | 22.273 | 22.391 | 22.092–23.362 | 0.265 | 22.636 | 23.960 |
| 176.4 | conservative | 20 | 22.364 | 22.337 | 22.092–22.636 | 0.159 | 22.454 | 23.949 |
| 192 | aggressive | 20 | 7.599 | 7.599 | 7.099–8.099 | 0.301 | 7.932 | 9.017 |
| 192 | balanced | 20 | 7.932 | 7.866 | 7.432–8.432 | 0.283 | 8.266 | 9.017 |
| 192 | conservative | 20 | 7.766 | 7.766 | 7.266–9.266 | 0.487 | 8.266 | 9.033 |

## Method and limits

- Physical analog output-to-input loopback with Logic closed. Standalone CoreAudio AUHAL client started and stopped for each probe; the transport remained running within each rate/profile group.
- Ordered rates: 44.1, 48, 88.2, 96, 176.4, 192 kHz. Ordered profiles: Aggressive, Balanced, Conservative. This was not a randomized profile comparison or a cold-boot benchmark.
- One rate-selection probe precedes each rate; wait 3 seconds afterward. Wait 2 seconds after setting each profile. These selection probes are separate from the 20 measured probes.
- Main latency is round_trip_seconds from AudioUnit timestamps. Callback wall time is a separate measurement affected by callback scheduling and buffer timing; it is not interchangeable with the timestamp result.
- No-signal results are excluded. Only results above 100 ms among the first three probes after a change qualify as transition-high exclusions. Later results above 100 ms, three consecutive no-signals, rate/profile mismatch, or deadline/restart/generation faults stop testing.
- Profile differences are small and their direction varies across rates. There is no consistently fastest profile in this run. No claim of statistically significant differences or host-load robustness is made.
- Queue figures in raw outputs are post-probe snapshots, not independent directional latency measurements.

## Earlier stop and retry

A preflight probe with Logic running measured 84.72 ms at 44.1 kHz; after closing Logic it measured 18.14 ms. The contaminated preflight was not part of the benchmark.

The first 48 kHz/Aggressive group was stopped after probe 12 reached 106.437 ms. Most preceding results stayed around 90 ms. Probe 1 (100.271 ms) was the single transition-high exclusion; probe 12 is retained as an anomalous successful reading. The transport reported advancing counters and zero rolling misses, while capture DBC gaps increased. The cause has not been established.

After user authorization to retry, all three 48 kHz profile groups completed cleanly with 20 successful probes each. These complete retry groups supply the main table. The earlier incomplete group is preserved in earlier-48-anomaly.csv and the original raw-output directory; it was not silently removed as an outlier.

## Runtime identity and final state

Source-tree and installed transport binary SHA-256 comparisons: all six match.

At completion the runner returned to 48 kHz/Aggressive, its starting state for the resumed benchmark. No source changes, reinstalls, resets for recovery, or commits were made. Profile choices were persisted by the existing CLI.

## Artifacts

- results.csv: 360 measurements, including wall time and queue snapshots, with links to raw files.
- earlier-48-anomaly.csv: all 12 readings from the stopped first 48 kHz group.
- summary.json: group statistics and installed/source binary hashes.
- Original per-run outputs, control checks and transport logs remain in the three source directories:
  - samples/benchmarks/fw1814-profiles-20260927-203624
  - samples/benchmarks/fw1814-48-retry-20260927-204149
  - samples/benchmarks/fw1814-remaining-profiles-20260927-204352
