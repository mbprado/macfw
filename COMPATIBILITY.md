# Compatibility matrix

This file records hardware-tested macOS compatibility for the macfw M-Audio FireWire 410 driver.

Only configurations that have actually been tested on real hardware are marked validated. Versions that appear likely to work based on neighboring macOS releases are still listed as untested until verified.

## M-Audio FireWire 410

| macOS version | Intel Mac | Installer | Playback | Capture | 44.1/48 kHz switching | Disconnect/reconnect | Status |
|---|---|---|---|---|---|---|---|
| Monterey 12.7.6 | validated | validated | validated | validated | validated | validated | **Validated** |
| Ventura 13.x | untested | untested | untested | untested | untested | untested | **Untested** |
| Sonoma 14.8.9 | validated | validated | validated | functional but degraded quality observed | validated functional test | validated functional test | **Functional, capture quality needs follow-up** |

## Monterey 12.7.6

The release package was installed on a freshly installed macOS Monterey 12.7.6 system.

Observed result:

- package installation completed normally;
- the HAL driver and launchd transport service became operational;
- the FW410 worked as expected after installation;
- normal playback and capture were confirmed;
- no pre-existing macfw development environment or driver installation was present.

This is currently the cleanest fully validated installation baseline.

## Sonoma 14.8.9

The release installer was also tested on macOS Sonoma 14.8.9.

Observed result:

- package installation completed normally;
- the interface was detected and managed correctly;
- playback worked;
- normal functional transport behavior was present;
- capture was functional but audibly somewhat broken/degraded during the quick functional test.

No capture tuning or transport-parameter investigation was performed during this Sonoma test. Therefore this result should not yet be treated as evidence of a Sonoma-specific transport defect; it is an open compatibility observation requiring controlled follow-up.

## Ventura 13.x

Ventura lies between the hardware-tested Monterey and Sonoma releases, but it has **not** yet been tested. Do not mark Ventura as validated or guaranteed compatible until a real installation and audio test is completed.

## Interpretation

Compatibility status in this file uses the following meanings:

- **Validated** — tested on real FW410 hardware with the packaged driver and expected audio behavior confirmed.
- **Functional, follow-up required** — installation and principal driver behavior work, but one or more quality/stability issues were observed.
- **Untested** — no hardware test has been performed, regardless of how likely compatibility may appear from adjacent macOS versions.

## Reporting additional results

When adding another compatibility result, record at minimum:

- exact macOS version;
- Intel Mac model;
- FireWire connection/adapters;
- package build/version;
- playback result;
- capture result;
- 44.1/48 kHz result;
- disconnect/reconnect result;
- any transport log anomalies.
