# FW1814 bring-up plan

This plan keeps the first FW1814 work conservative and separates observed hardware facts from published reference information.

## Phase 0 — read-only fingerprint

Collect from the actual development unit without issuing control or streaming writes:

- FireWire Product Name;
- Vendor_ID;
- GUID;
- Unit_Spec_ID;
- Unit_SW_Version;
- configuration ROM;
- BeBoB information block at `0xffffc8020000`;
- hardware model/revision;
- software build date/time, ID and revision;
- bootloader build date/time and version.

Only after these values are captured should operational/bootloader identity matches be added to `devices/fw1814/profile.h` or the production installer registry.

## Phase 1 — 48 kHz receive/capture

- verify operational firmware state;
- inspect current AV/C signal formats and CMP plug state;
- determine the 48 kHz receive stream formation on the actual unit;
- start receive-only isochronous transport;
- decode AM824 labels/DBC/SYT without exposing CoreAudio yet;
- map analog inputs first;
- then identify S/PDIF/ADAT positions for the selected digital-input mode;
- preserve/ignore MIDI positions without implementing CoreMIDI.

No mixer/configuration writes are required for this phase unless the interface cannot enter a known internal-clock/S/PDIF mode safely through a documented operation.

## Phase 2 — 48 kHz playback and full duplex

- parameterize the proven macfw TX packet geometry instead of cloning FW410 constants;
- map physical analog outputs first;
- validate tone/silence transport;
- add continuous PCM playback;
- combine receive + transmit using the proven dedicated real-time audio service model;
- establish a stable low-latency baseline before CoreAudio integration.

## Phase 3 — 44.1 kHz

- reuse the proven native-44.1 packet scheduler where possible;
- determine whether the FW1814 requires any device-specific post-start rate handling;
- validate repeated 44.1/48 transitions before integrating rate changes with CoreAudio.

Do not assume the FW410 post-start 44.1 reassert is required on the FW1814 until hardware evidence shows it.

## Phase 4 — shared HAL/CoreAudio architecture

Generalize the currently FW410-shaped shared-memory/HAL constants into a device profile:

- playback/capture channel counts;
- CoreAudio channel names/order;
- per-device shared-memory identities;
- stream channel maps;
- supported rates;
- device name/UID;
- packet geometry supplied to the common transport.

The goal is one reusable HAL/transport architecture with per-device profiles, not a copied FW1814 HAL.

## Phase 5 — FW1814 controls

Only after audio is stable:

- implement the documented M-Audio special-firmware configuration backend;
- use only known-safe memory/register writes;
- cache write-only state in software;
- add clock source and S/PDIF/ADAT mode controls;
- add mixer/output/headphone/AUX controls incrementally;
- expose hardware meters after their read format is validated.

The FW1814 special firmware is known to react badly to unsupported commands. Exploratory control writes must therefore stay out of the production transport path.

## Deferred

- MIDI/CoreMIDI;
- 88.2 kHz;
- 96 kHz;
- 176.4 kHz;
- 192 kHz;
- ADAT S/MUX/high-rate mode work;
- signing/notarization changes unrelated to device bring-up.
